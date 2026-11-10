#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/initial_scene_projection.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/program_reference_validator.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "content_identity_internal.hpp"
#include "program_binding_internal.hpp"
#include "program_root_fixture.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "gripper_only_retention_fixture.hpp"
#include "pneumatic_program_fixture.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#include "io_retained_fixture.hpp"
#include "program_binding_internal.hpp"

namespace {

using manyforge::core::CapabilityId;
using manyforge::deployment::ActiveRuntimeParticipantFactsDTO;
using manyforge::deployment::build_runtime_participant_binding_dto;
using manyforge::deployment::canonical_json;
using manyforge::deployment::CanonicalValue;
using manyforge::deployment::compile_direct_control_gripper_program_binding;
using manyforge::deployment::compile_direct_control_motion_program_binding;
using manyforge::deployment::compile_direct_control_program_binding_inventory;
using manyforge::deployment::compile_program_binding;
using manyforge::deployment::compile_program_binding_outcome;
using manyforge::deployment::DecodeProgramNonExecutableOutcome;
using manyforge::deployment::EncodeProgramNonExecutableOutcome;
using manyforge::deployment::ProgramBindingUnresolvedReason;
using manyforge::deployment::FilesystemCatalogArtifactStore;
using manyforge::deployment::FilesystemVerifiedRetainedBundleSource;
using manyforge::deployment::FilesystemVerifiedRetainedModelMapSource;
using manyforge::deployment::ProgramBindingCompilerError;
using manyforge::deployment::resolve_runtime_program_target;
using manyforge::deployment::RetainedBundle;
using manyforge::deployment::RetainedModelMapQuery;
using manyforge::deployment::RuntimeProgramBindingResolutionError;
using manyforge::deployment::RuntimeProviderEpochFactDTO;
using manyforge::deployment::VerifiedRetainedModelMap;
using manyforge::deployment::verify_program_binding_contract;
using manyforge::deployment::detail::compute_content_identity;
using manyforge::deployment::detail::ContentIdentitySpec;
using manyforge::deployment::test::kLite6ArmTargetId;
using manyforge::deployment::test::kLite6ControllableComponentId;
using manyforge::deployment::test::kLite6GripperTargetId;
using manyforge::deployment::test::kLiteGripperComponentId;
using manyforge::deployment::test::
    materialize_lite6_ros2_control_observed_retained_consumer_fixture;
using manyforge::deployment::test::
    materialize_lite6_ros2_control_observed_endpoint_retained_consumer_fixture;
using manyforge::deployment::test::
    materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

class ScopedTrustedRoot final {
public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-program-binding-compiler-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error("test could not create trusted root");
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

struct BindingCompilerContext final {
  std::shared_ptr<const RetainedBundle> bundle;
  VerifiedRetainedModelMap model_map;
};

[[nodiscard]] BindingCompilerContext
make_context(const std::filesystem::path &root, bool gripper_only = false,
             bool endpoint_contact_bodies = false,
             bool shared_arm_endpoint_bodies = false,
             bool gripper_feedback = false) {
  const auto materialized = [&] {
    if (!gripper_only) {
      if (endpoint_contact_bodies) {
        return materialize_lite6_ros2_control_observed_endpoint_retained_consumer_fixture(
            root, shared_arm_endpoint_bodies);
      }
      return materialize_lite6_ros2_control_observed_retained_consumer_fixture(root);
    }
    // The Lite gripper as its own component (a separate tool). Its
    // `gripper.command_position` target also needs tool feedback.
    return manyforge::deployment::test::materialize_lite6_retained_consumer_fixture_from(
        root, manyforge::deployment::test::make_lite_gripper_only_retention_fixture(
                  manyforge::deployment::test::kWithLifecycleOffers, "pneumatic",
                  std::nullopt, "commanded", gripper_feedback));
  }();
  const FilesystemVerifiedRetainedBundleSource bundle_source(root);
  auto bundle =
      bundle_source.get_verified(materialized.bundle->retained_bundle_id());
  if (bundle == nullptr ||
      bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
    throw std::logic_error("test retained bundle has no sole model map");
  }
  const auto &product =
      bundle->value().compiled_products.symbol_role_maps.front();
  const FilesystemVerifiedRetainedModelMapSource model_map_source(root);
  auto model_map = model_map_source.get_verified(
      RetainedModelMapQuery{bundle->retained_bundle_id(), product.product_id});
  if (!model_map.has_value()) {
    throw std::logic_error("test could not reload retained model map");
  }
  return {std::move(bundle), std::move(*model_map)};
}

[[nodiscard]] BindingCompilerContext
make_ros_motion_context(const std::filesystem::path &root) {
  const auto materialized =
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          root, "lite6_motion_cell");
  const FilesystemVerifiedRetainedBundleSource bundle_source(root);
  auto bundle =
      bundle_source.get_verified(materialized.bundle->retained_bundle_id());
  if (bundle == nullptr ||
      bundle->value().compiled_products.symbol_role_maps.size() != 1U) {
    throw std::logic_error("test retained bundle has no sole model map");
  }
  const auto &product =
      bundle->value().compiled_products.symbol_role_maps.front();
  const FilesystemVerifiedRetainedModelMapSource model_map_source(root);
  auto model_map = model_map_source.get_verified(
      RetainedModelMapQuery{bundle->retained_bundle_id(), product.product_id});
  if (!model_map.has_value()) {
    throw std::logic_error("test could not reload retained model map");
  }
  return {std::move(bundle), std::move(*model_map)};
}

/// The maintained Lite6 product: its cell-global execution, collision and
/// target capabilities come from the real activation path, not hand-built rows.
[[nodiscard]] BindingCompilerContext
make_maintained_motion_context(const std::filesystem::path &root) {
  auto product = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root, manyforge::deployment::parse_strict_json_model_yaml(
                R"({"interrupt_rules":[]})").root());
  if (!product.bundle || !product.model_map) {
    throw std::logic_error("maintained Lite6 product has no bundle or model map");
  }
  return {std::move(product.bundle), std::move(*product.model_map)};
}

[[nodiscard]] CanonicalValue
make_program_root(const RetainedBundle &bundle,
                  bool include_node_policy = false,
                  bool own_group_intervention = false,
                  bool planning_only = false) {
  CanonicalValue::Object tree{
      {"node_id", CanonicalValue("sequence")},
      {"instance_id", CanonicalValue("root")},
      {"parameters", CanonicalValue(CanonicalValue::Object{})},
      {"children", CanonicalValue(CanonicalValue::Array{})},
  };
  if (include_node_policy) {
    CanonicalValue::Object recovery{
        {"allow_intervention", CanonicalValue(true)},
        {"max_retries", CanonicalValue(1.0)},
    };
    if (own_group_intervention) {
      recovery.emplace_back("intervention_stop_scope",
                            CanonicalValue("own_groups"));
    }
    tree.emplace_back("recovery_policy", CanonicalValue(std::move(recovery)));
    tree.emplace_back("operator_stop_policy", CanonicalValue("hold"));
  }
  const CanonicalValue::Array required_capabilities = planning_only
      ? CanonicalValue::Array{
            CanonicalValue(CanonicalValue::Object{
                {"capability_id", CanonicalValue("planning.motion.pose_goal")},
                {"parameters", CanonicalValue(CanonicalValue::Object{})},
                {"disposition", CanonicalValue("required")},
            }),
            CanonicalValue(CanonicalValue::Object{
                {"capability_id", CanonicalValue("state.observed.current")},
                {"parameters", CanonicalValue(CanonicalValue::Object{})},
                {"disposition", CanonicalValue("required")},
            }),
        }
      : CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"capability_id", CanonicalValue("state.observed.current")},
            {"parameters", CanonicalValue(CanonicalValue::Object{})},
            {"disposition", CanonicalValue("required")},
        })};
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.program.v2")},
      {"program_id", CanonicalValue("binding_compiler_program")},
      {"program_revision", CanonicalValue(std::string(kZeroDigest))},
      {"program_profile", CanonicalValue("composable-cell-v1")},
      {"name", CanonicalValue("Binding compiler fixture")},
      {"catalog",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.node_catalog.v3")},
           {"content_version",
            CanonicalValue(
                bundle.value().catalogs.node_environment.content_version)},
       })},
      {"component_roles",
       CanonicalValue(CanonicalValue::Object{
           {"robot", CanonicalValue(CanonicalValue::Object{})},
       })},
      {"target_roles",
       CanonicalValue(CanonicalValue::Object{
           {"arm",
            CanonicalValue(CanonicalValue::Object{
                {"component_role", CanonicalValue("robot")},
                {"required_capabilities", CanonicalValue(required_capabilities)},
                {"required_frame_roles",
                 CanonicalValue(CanonicalValue::Array{CanonicalValue("tcp")})},
                {"required_resource_roles",
                 CanonicalValue(
                     CanonicalValue::Array{CanonicalValue("joints")})},
                {"required_collision_body_roles",
                 CanonicalValue(
                     CanonicalValue::Array{CanonicalValue("arm_collision")})},
                {"required_model_roles", CanonicalValue(CanonicalValue::Array{
                                             CanonicalValue("arm_model")})},
            })},
       })},
      {"tree", CanonicalValue(std::move(tree))},
  });
}

/// A v3 Program over the exact Lite gripper and its collision body.
[[nodiscard]] CanonicalValue
make_gripper_program_root(const RetainedBundle &bundle) {
  manyforge::deployment::test::ProgramOptions options;
  options.catalog_content_version = bundle.value().catalogs.node_environment.content_version;
  options.include_contact_grasp = true;
  auto root = manyforge::deployment::test::make_program_root(options);
  auto policy = root.find_object_member("contact_policies")->as_array().front();
  auto zones = policy.find_object_member("zones")->as_array();
  auto zone = zones.front();
  auto pairs = zone.find_object_member("body_pairs")->as_array();
  auto pair = pairs.front();
  auto body_a = *pair.find_object_member("body_a");
  const auto replace = [](CanonicalValue object, std::string_view name, CanonicalValue value) {
    auto fields = object.as_object();
    for (auto &[key, selected] : fields) if (key == name) { selected = std::move(value); return CanonicalValue(std::move(fields)); }
    fields.emplace_back(std::string(name), std::move(value));
    return CanonicalValue(std::move(fields));
  };
  body_a = replace(body_a, "body", CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(std::string(kLiteGripperComponentId))},
      {"collision_body_id", CanonicalValue("gripper_collision")}}));
  pair = replace(pair, "body_a", std::move(body_a));
  pairs.front() = std::move(pair);
  zone = replace(zone, "body_pairs", CanonicalValue(std::move(pairs)));
  zones.front() = std::move(zone);
  policy = replace(policy, "zones", CanonicalValue(std::move(zones)));
  policy = replace(policy, "version", CanonicalValue(compute_content_identity(
      policy, ContentIdentitySpec{"catalog-record-revision/contact_policy", "manyforge.contact_policy.v1", "version"})));
  root = replace(root, "contact_policies", CanonicalValue(CanonicalValue::Array{std::move(policy)}));
  root = replace(root, "tree", manyforge::deployment::parse_strict_json_model_yaml(R"({
      "node_id":"gripper.command_position","instance_id":"close","children":[],
      "parameters":{"target":{"component_id":"lite_gripper","command_target_id":"gripper"},
      "position":{"kind":"named_preset","name":"closed"}}})").root());
  return manyforge::deployment::test::with_program_revision(manyforge::deployment::normalize_program_authoring(root));
}

[[nodiscard]] std::string
make_program_bytes(const RetainedBundle &bundle,
                   bool include_node_policy = false,
                   bool own_group_intervention = false,
                   bool planning_only = false) {
  CanonicalValue root = make_program_root(bundle, include_node_policy,
                                          own_group_intervention,
                                          planning_only);
  const std::string revision =
      compute_content_identity(root, ContentIdentitySpec{
                                         .identity_domain = "program-revision",
                                         .api_version = "manyforge.program.v2",
                                         .own_field = "program_revision",
                                     });
  CanonicalValue::Object members = root.as_object();
  for (auto &[name, value] : members) {
    if (name == "program_revision") {
      value = CanonicalValue(revision);
      return canonical_json(CanonicalValue(std::move(members)));
    }
  }
  throw std::logic_error("test Program omitted program_revision");
}

[[nodiscard]] std::string direct_motion_program(const RetainedBundle &bundle, bool policy = false) {
  manyforge::deployment::test::ProgramOptions options;
  options.catalog_content_version = bundle.value().catalogs.node_environment.content_version;
  auto fields = manyforge::deployment::test::make_program_root(options).as_object();
  auto motion = manyforge::deployment::parse_strict_json_model_yaml(R"({
      "node_id":"motion.move_to_joint_target","instance_id":"move","children":[],
      "parameters":{"target":{"component_id":"lite6_robot","command_target_id":"arm"},
        "base_frame":{"component_id":"lite6_robot","frame_id":"base"},
        "tcp_endpoint":{"kind":"current"},"goal":[{"joint_name":"joint1","position_rad":0.0}]}})").root();
  if (policy) {
    auto node = motion.as_object();
    node.emplace_back("recovery_policy", CanonicalValue(CanonicalValue::Object{
        {"allow_intervention", CanonicalValue(true)}, {"max_retries", CanonicalValue(1.0)},
        {"intervention_stop_scope", CanonicalValue("own_groups")}}));
    node.emplace_back("operator_stop_policy", CanonicalValue("hold"));
    motion = CanonicalValue(std::move(node));
  }
  for (auto &[name, value] : fields) if (name == "tree") value = std::move(motion);
  return canonical_json(manyforge::deployment::test::with_program_revision(
      manyforge::deployment::normalize_program_authoring(CanonicalValue(std::move(fields)))));
}

[[nodiscard]] ActiveRuntimeParticipantFactsDTO active_facts_for(
    const manyforge::deployment::RuntimeProgramTargetBinding &target) {
  if (!target.desired_route().has_value()) {
    throw std::logic_error("test target has no desired route");
  }
  std::set<manyforge::core::RuntimeUnitId> runtime_units(
      target.component().runtime_unit_ids.begin(),
      target.component().runtime_unit_ids.end());
  if (target.control_domain().has_value()) {
    runtime_units.insert(target.control_domain()->runtime_unit_ids.begin(),
                         target.control_domain()->runtime_unit_ids.end());
  }
  std::vector<manyforge::core::RuntimeUnitEpochBindingDTO> runtime_epochs;
  for (const auto &id : runtime_units) {
    runtime_epochs.push_back(
        {id, manyforge::core::RuntimeEpoch("epoch." + id.value())});
  }

  std::map<manyforge::core::ProviderInstanceId, manyforge::core::RuntimeUnitId>
      providers;
  if (target.control_binding().has_value()) {
    providers.emplace(target.control_binding()->provider_instance_id,
                      target.control_binding()->provider_runtime_unit_id);
  }
  if (target.capability_provider_binding().has_value()) {
    const auto &provider = *target.capability_provider_binding();
    const auto endpoint = std::find_if(
        provider.endpoint_bindings.begin(), provider.endpoint_bindings.end(),
        [](const auto &candidate) {
          return candidate.source_kind ==
                 manyforge::core::CapabilityEndpointSourceKind::kRuntimeUnit;
        });
    if (endpoint == provider.endpoint_bindings.end()) {
      throw std::logic_error(
          "test provider has no managed runtime-unit source");
    }
    const auto *runtime_unit =
        std::get_if<manyforge::core::RuntimeUnitId>(&endpoint->source_id);
    if (runtime_unit == nullptr) {
      throw std::logic_error("test provider runtime source is ill-typed");
    }
    providers.emplace(provider.provider_instance_id, *runtime_unit);
    runtime_units.insert(*runtime_unit);
  }
  std::vector<RuntimeProviderEpochFactDTO> provider_epochs;
  for (const auto &[id, runtime_unit_id] : providers) {
    provider_epochs.push_back(
        {id, runtime_unit_id,
         manyforge::core::RuntimeEpoch("epoch." + runtime_unit_id.value())});
  }
  return ActiveRuntimeParticipantFactsDTO{
      *target.desired_route(),
      manyforge::core::ActiveControlConfigurationId("active.native"),
      manyforge::core::ActiveControlConfigurationGeneration(1U),
      std::move(runtime_epochs), std::move(provider_epochs)};
}

TEST(ProgramBindingCompilerTest,
     ResolvesOneProgramAgainstExactRetainedComponentTargetAndCatalog) {
  ScopedTrustedRoot root;
  const auto context = make_maintained_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog(root.path());
  const auto contract = verify_program_binding_contract(direct_motion_program(*context.bundle), *context.bundle, context.model_map, catalog);
  const auto binding = compile_program_binding(*contract, *context.bundle, context.model_map, catalog);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->value().program_id, contract->value().program_id);
  EXPECT_EQ(binding->value().program_revision, contract->value().program_revision);
  EXPECT_EQ(binding->value().retained_bundle_id, context.bundle->retained_bundle_id());
  // The arm's maintained TCP inventory carries the mounted gripper's `tcp`
  // frame, so the gripper component is bound alongside the arm.
  const auto &components = binding->value().component_bindings;
  ASSERT_EQ(components.size(), 2U);
  EXPECT_TRUE(std::any_of(components.begin(), components.end(), [](const auto &row) {
    return row.component_id.value() == "lite6_robot";
  }));
  EXPECT_TRUE(std::any_of(components.begin(), components.end(), [](const auto &row) {
    return row.component_id.value() == "lite_gripper";
  }));
  ASSERT_EQ(binding->value().target_bindings.size(), 1U);
  const auto &target = binding->value().target_bindings.front();
  EXPECT_EQ(target.target.command_target_id.value(), kLite6ArmTargetId);
  EXPECT_FALSE(target.frame_references.empty());
  EXPECT_FALSE(target.resource_role_bindings.empty());
  const auto state = std::find_if(target.capability_bindings.begin(), target.capability_bindings.end(),
      [](const auto &capability) { return capability.capability_id.value() == "state.observed.current"; });
  ASSERT_NE(state, target.capability_bindings.end());
  EXPECT_TRUE(state->control_binding.has_value());
  EXPECT_FALSE(state->command_schema.has_value());
  EXPECT_EQ(binding->value().provenance.source_records, context.bundle->value().provenance.source_records);
  EXPECT_FALSE(binding->value().provenance.resolved_output_records.empty());
}

// v3 has no independently declared planning-only target role. The exact
// retained capabilities and selectable TCP inventory are still checked.
TEST(ProgramBindingCompilerTest, PlanningCapabilityAndTcpInventoryRemainAvailable) {
  ScopedTrustedRoot root;
  const auto context = make_ros_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog(root.path());
  // The motion fixture carries no StateObserved binding; that capability is
  // qualified only by the observed-state fixture.
  ScopedTrustedRoot observed_root;
  const auto observed = make_context(observed_root.path());
  FilesystemCatalogArtifactStore observed_catalog(observed_root.path());
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const manyforge::core::CommandTargetId target{std::string(kLite6ArmTargetId)};
  for (const auto *id : {"planning.motion.pose_goal", "state.observed.current"}) {
    const auto *definition = manyforge::core::find_builtin_capability_definition(id);
    ASSERT_NE(definition, nullptr);
    const manyforge::core::ProgramCapabilityRequirementDTO requirement{
        CapabilityId(id), manyforge::core::CapabilityDefinitionRevision(
            std::string(definition->capability_definition_revision)),
        manyforge::core::ProgramCapabilityDisposition::kRequired};
    const bool observed_state = std::string_view(id) == "state.observed.current";
    const auto &probe = observed_state ? observed : context;
    const auto &probe_catalog = observed_state ? observed_catalog : catalog;
    EXPECT_FALSE(manyforge::deployment::probe_program_target_capability(
        component, target, requirement, *probe.bundle, probe.model_map, probe_catalog));
  }
  const auto binding = compile_direct_control_motion_program_binding(
      component, target, *context.bundle, context.model_map, catalog);
  ASSERT_NE(binding, nullptr);
  ASSERT_EQ(binding->value().target_bindings.size(), 1U);
  const auto &row = binding->value().target_bindings.front();
  ASSERT_TRUE(row.robot_tcps);
  ASSERT_EQ(row.robot_tcps->size(), 2U);
  EXPECT_EQ(row.robot_tcps->front().entry.frame.frame_id.value(), "flange");
  EXPECT_EQ(row.robot_tcps->back().entry.frame.frame_id.value(), "tcp");
}

TEST(ProgramBindingCompilerTest, CopiesVerifiedNodePoliciesAndLeavesDirectControlEmpty) {
  ScopedTrustedRoot root;
  const auto context = make_maintained_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog(root.path());
  const auto contract = verify_program_binding_contract(direct_motion_program(*context.bundle, true), *context.bundle, context.model_map, catalog);
  const auto binding = compile_program_binding(*contract, *context.bundle, context.model_map, catalog);
  ASSERT_EQ(contract->value().node_policies.size(), 1U);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->value().node_policies, contract->value().node_policies);
  EXPECT_EQ(binding->value().node_policies.front().recovery_policy.intervention_stop_scope,
            manyforge::core::ProgramInterventionStopScope::kOwnGroups);
  const auto direct = compile_direct_control_motion_program_binding(
      manyforge::core::ControllableComponentId("lite6_robot"),
      manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
      *context.bundle, context.model_map, catalog);
  ASSERT_NE(direct, nullptr);
  EXPECT_TRUE(direct->value().node_policies.empty());
}

TEST(ProgramBindingCompilerTest,
     DerivesDeterministicDirectControlMotionBindingWithoutScenarioProgram) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context =
      make_ros_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());

  const auto first = compile_direct_control_motion_program_binding(
      manyforge::core::ControllableComponentId(
          std::string(kLite6ControllableComponentId)),
      manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
      *context.bundle, context.model_map, catalog_store);
  const auto second = compile_direct_control_motion_program_binding(
      manyforge::core::ControllableComponentId(
          std::string(kLite6ControllableComponentId)),
      manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
      *context.bundle, context.model_map, catalog_store);

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->program_binding_id(), second->program_binding_id());
  EXPECT_EQ(first->canonical_bytes(), second->canonical_bytes());
  EXPECT_EQ(first->value().program_id.value(),
            manyforge::deployment::kDirectControlMotionProgramId);
  EXPECT_TRUE(manyforge::deployment::is_sha256_digest(
      first->value().program_revision.value()));
  EXPECT_EQ(first->value().retained_bundle_id,
            context.bundle->retained_bundle_id());
  ASSERT_EQ(first->value().component_bindings.size(), 1U);
  EXPECT_EQ(first->value().component_bindings.front().component_id.value(),
            kLite6ControllableComponentId);
  ASSERT_EQ(first->value().target_bindings.size(), 1U);
  const auto &target = first->value().target_bindings.front();
  EXPECT_EQ(target.target.component_id.value(), kLite6ControllableComponentId);
  EXPECT_EQ(target.target.command_target_id.value(), kLite6ArmTargetId);
  EXPECT_FALSE(target.frame_references.empty());
  EXPECT_NE(std::find_if(
                target.frame_references.begin(),
                target.frame_references.end(),
                [](const auto &frame) {
                  return frame.frame_id.value() ==
                         manyforge::deployment::kDirectControlBaseFrameRoleId;
                }),
            target.frame_references.end());
  // The binding holds `base` and exactly the target's tagged TCPs
  // (`flange` and `tcp` on this arm, as on the maintained Lite6 arm).
  std::vector<std::string> frame_roles;
  for (const auto &frame : target.frame_references)
    frame_roles.push_back(frame.frame_id.value());
  EXPECT_EQ(frame_roles, (std::vector<std::string>{"base", "flange", "tcp"}));
  ASSERT_EQ(target.frame_associations.size(), target.frame_references.size());
  for (std::size_t i = 0; i < target.frame_references.size(); ++i)
    EXPECT_EQ(target.frame_associations[i].frame, target.frame_references[i]);
  EXPECT_EQ(first->canonical_bytes().find("\"target_role_id\""), std::string::npos);
  EXPECT_EQ(first->canonical_bytes().find("\"component_role_id\""), std::string::npos);
  EXPECT_FALSE(target.resource_role_bindings.empty());
  EXPECT_NE(std::find_if(target.resource_role_bindings.begin(), target.resource_role_bindings.end(),
                         [](const auto &row) { return row.resource_role_id.value() == "joints"; }),
            target.resource_role_bindings.end());
  EXPECT_FALSE(target.collision_body_references.empty());
  EXPECT_TRUE(first->value().contact_grasp_bindings.empty());

  std::vector<std::string> capabilities;
  for (const auto &capability : target.capability_bindings) {
    capabilities.push_back(capability.capability_id.value());
  }
  EXPECT_EQ(capabilities,
            (std::vector<std::string>{"execution.trajectory.execute",
                                      "planning.motion.cartesian_path",
                                      "planning.motion.joint_goal",
                                      "planning.motion.pose_goal"}));
}

// A target frame role binds the target's own row, and an absent role
// binds nothing; the direct-control binding holds `base` and the target's
// tagged TCPs.
TEST(ProgramBindingCompilerTest, TargetFrameRolesBindTheTargetsOwnRow) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_ros_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const manyforge::core::ControllableComponentId component{
      std::string(kLite6ControllableComponentId)};
  const auto *target = context.model_map.find_target(
      component, manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)));
  ASSERT_NE(target, nullptr);
  ASSERT_TRUE(context.bundle->value().cell_graph.component_attachments.empty());
  for (const std::string_view role :
       {std::string_view("tcp"), std::string_view("flange"),
        manyforge::deployment::kDirectControlBaseFrameRoleId}) {
    const manyforge::core::FrameRoleId id{std::string(role)};
    const auto own = std::find_if(
        target->frames.begin(), target->frames.end(),
        [&](const auto &frame) { return frame.frame_role_id == id; });
    ASSERT_NE(own, target->frames.end());
    EXPECT_EQ(manyforge::deployment::find_target_frame_role(*target, id), &*own);
  }
  EXPECT_EQ(manyforge::deployment::find_target_frame_role(
                *target, manyforge::core::FrameRoleId("cup_center")),
            nullptr);
  const auto direct = compile_direct_control_motion_program_binding(
      component, manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
      *context.bundle, context.model_map, catalog_store);
  ASSERT_NE(direct, nullptr);
  EXPECT_EQ(manyforge::deployment::reload_verified_program_binding(
                direct->canonical_bytes(), *context.bundle, context.model_map, catalog_store)
                ->value(),
            direct->value());
  auto forged = direct->value();
  ASSERT_TRUE(forged.target_bindings.front().robot_tcps);
  ASSERT_FALSE(forged.target_bindings.front().robot_tcps->empty());
  forged.target_bindings.front().robot_tcps->front().frame.compiled_frame_name =
      "foreign_tcp_frame";
  const auto resealed = manyforge::deployment::detail::
      ProgramBindingConstructionAuthority::seal(std::move(forged));
  EXPECT_THROW(static_cast<void>(manyforge::deployment::reload_verified_program_binding(
                   resealed->canonical_bytes(), *context.bundle, context.model_map, catalog_store)),
               manyforge::deployment::ProgramBindingError);
  const auto &frames = direct->value().target_bindings.front().frame_references;
  std::vector<std::string> roles;
  for (const auto &frame : frames) {
    EXPECT_EQ(frame.component_id, component);
    roles.push_back(frame.frame_id.value());
  }
  EXPECT_EQ(roles, (std::vector<std::string>{"base", "flange", "tcp"}));
  for (const auto &association : direct->value().target_bindings.front().frame_associations) {
    const auto matches = manyforge::deployment::find_program_frame_matches(
        association.frame, context.model_map);
    ASSERT_EQ(matches.size(), 1U);
    const auto &physical = matches.front();
    EXPECT_TRUE(std::any_of(target->frames.begin(), target->frames.end(),
        [&](const auto &candidate) {
          const auto *frame = std::get_if<manyforge::deployment::CompositionComponentModelFrame>(
              &candidate.frame);
          return frame && frame->controllable_component_id == association.frame.component_id &&
                 frame->model_instance_id == physical.model_instance_id &&
                 frame->model_revision == physical.model_revision &&
                 frame->compiled_frame_name == physical.compiled_frame_name;
        }));
    for (const auto &body_id : association.contact_body_role_ids) {
      const manyforge::core::ProgramCollisionBodyReferenceDTO reference{
          association.frame.component_id, body_id};
      const auto bodies = manyforge::deployment::find_program_collision_body_matches(
          reference, context.model_map);
      ASSERT_EQ(bodies.size(), 1U);
      EXPECT_EQ(bodies.front().model_instance_id, physical.model_instance_id);
      EXPECT_EQ(bodies.front().model_revision, physical.model_revision);
      EXPECT_NE(std::find(direct->value().target_bindings.front().collision_body_references.begin(),
                          direct->value().target_bindings.front().collision_body_references.end(),
                          reference),
                direct->value().target_bindings.front().collision_body_references.end());
    }
  }
}

// A declared component alias and its model ID can name the same physical
// frame. Target-local binding must retain the authored alias and exact model.
TEST(ProgramBindingCompilerTest, DirectTargetFrameAliasUsesQualifiedPhysicalFrame) {
  ScopedTrustedRoot root;
  const auto materialized = manyforge::deployment::test::materialize_lite6_retained_consumer_fixture_from(
      root.path(), manyforge::deployment::test::make_lite6_ros2_control_observed_consumer_retention_fixture(
                       false, false, "flange"));
  const FilesystemVerifiedRetainedBundleSource bundle_source(root.path());
  const auto bundle = bundle_source.get_verified(materialized.bundle->retained_bundle_id());
  ASSERT_NE(bundle, nullptr);
  ASSERT_EQ(bundle->value().compiled_products.symbol_role_maps.size(), 1U);
  const auto &product = bundle->value().compiled_products.symbol_role_maps.front();
  const FilesystemVerifiedRetainedModelMapSource map_source(root.path());
  const auto map = map_source.get_verified(RetainedModelMapQuery{bundle->retained_bundle_id(), product.product_id});
  ASSERT_TRUE(map);
  FilesystemCatalogArtifactStore catalog(root.path());
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const manyforge::core::CommandTargetId target{std::string(kLite6ArmTargetId)};
  const manyforge::core::ProgramFrameReferenceDTO alias{component, manyforge::core::FrameRoleId("flange")};
  const manyforge::core::ProgramFrameReferenceDTO model_id{component, manyforge::core::FrameRoleId("flange")};
  const auto alias_frames = manyforge::deployment::find_program_frame_matches(alias, *map);
  const auto model_frames = manyforge::deployment::find_program_frame_matches(model_id, *map);
  ASSERT_EQ(alias_frames.size(), 1U);
  ASSERT_EQ(model_frames.size(), 1U);
  EXPECT_EQ(alias_frames.front().model_instance_id, model_frames.front().model_instance_id);
  EXPECT_EQ(alias_frames.front().model_revision, model_frames.front().model_revision);
  EXPECT_EQ(alias_frames.front().compiled_frame_name, model_frames.front().compiled_frame_name);
  const auto binding = compile_direct_control_motion_program_binding(component, target, *bundle, *map, catalog);
  ASSERT_NE(binding, nullptr);
  ASSERT_EQ(binding->value().target_bindings.size(), 1U);
  const auto &associations = binding->value().target_bindings.front().frame_associations;
  EXPECT_NE(std::find_if(associations.begin(), associations.end(),
                         [&](const auto &row) { return row.frame == alias; }), associations.end());
  const auto &tcps = binding->value().target_bindings.front().robot_tcps;
  ASSERT_TRUE(tcps);
  const auto alias_tcp = std::find_if(tcps->begin(), tcps->end(),
      [&](const auto &row) { return row.entry.frame == alias; });
  ASSERT_NE(alias_tcp, tcps->end());
  EXPECT_EQ(alias_tcp->frame.model_instance_id, alias_frames.front().model_instance_id);
  EXPECT_EQ(alias_tcp->frame.model_revision, alias_frames.front().model_revision);
  EXPECT_EQ(alias_tcp->frame.compiled_frame_name, alias_frames.front().compiled_frame_name);
  const auto &frames = binding->value().frame_bindings;
  const auto found = std::find_if(frames.begin(), frames.end(),
                                  [&](const auto &row) { return row.reference == alias; });
  ASSERT_NE(found, frames.end());
  EXPECT_EQ(found->model_instance_id, alias_frames.front().model_instance_id);
  EXPECT_EQ(found->model_revision, alias_frames.front().model_revision);
  EXPECT_EQ(found->compiled_frame_name, alias_frames.front().compiled_frame_name);
  const auto model_row = std::find_if(frames.begin(), frames.end(),
                                      [&](const auto &row) { return row.reference == model_id; });
  ASSERT_NE(model_row, frames.end());
  EXPECT_EQ(model_row->model_instance_id, found->model_instance_id);
  EXPECT_EQ(model_row->model_revision, found->model_revision);
  EXPECT_EQ(model_row->compiled_frame_name, found->compiled_frame_name);
  const auto reloaded = manyforge::deployment::reload_verified_program_binding(
      binding->canonical_bytes(), *bundle, *map, catalog);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->value(), binding->value());
}

TEST(ProgramBindingCompilerTest, DirectDefaultTcpKeepsDeclaredAlias) {
  ScopedTrustedRoot root;
  const auto materialized = manyforge::deployment::test::materialize_lite6_retained_consumer_fixture_from(
      root.path(), manyforge::deployment::test::make_lite6_ros2_control_observed_consumer_retention_fixture(
                       false, false, "flange", true));
  const FilesystemVerifiedRetainedBundleSource bundle_source(root.path());
  const auto bundle = bundle_source.get_verified(materialized.bundle->retained_bundle_id());
  ASSERT_NE(bundle, nullptr);
  ASSERT_EQ(bundle->value().compiled_products.symbol_role_maps.size(), 1U);
  const auto &product = bundle->value().compiled_products.symbol_role_maps.front();
  const FilesystemVerifiedRetainedModelMapSource map_source(root.path());
  const auto map = map_source.get_verified(RetainedModelMapQuery{bundle->retained_bundle_id(), product.product_id});
  ASSERT_TRUE(map);
  FilesystemCatalogArtifactStore catalog(root.path());
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const manyforge::core::CommandTargetId target_id{std::string(kLite6ArmTargetId)};
  const manyforge::core::ProgramFrameReferenceDTO alias{component, manyforge::core::FrameRoleId("flange")};
  const auto binding = compile_direct_control_motion_program_binding(component, target_id, *bundle, *map, catalog);
  ASSERT_NE(binding, nullptr);
  ASSERT_EQ(binding->value().target_bindings.size(), 1U);
  const auto &target = binding->value().target_bindings.front();
  ASSERT_TRUE(target.default_tcp);
  EXPECT_EQ(target.default_tcp->target, (manyforge::core::ProgramTargetReferenceDTO{component, target_id}));
  EXPECT_EQ(target.default_tcp->frame, alias);
  ASSERT_TRUE(target.robot_tcps);
  const auto selected = std::find_if(target.robot_tcps->begin(), target.robot_tcps->end(),
      [&](const auto &tcp) { return tcp.entry == *target.default_tcp; });
  ASSERT_NE(selected, target.robot_tcps->end());
  EXPECT_TRUE(selected->component_default);
  const auto physical = manyforge::deployment::find_program_frame_matches(alias, *map);
  ASSERT_EQ(physical.size(), 1U);
  EXPECT_EQ(selected->frame, physical.front());
  const auto reloaded = manyforge::deployment::reload_verified_program_binding(
      binding->canonical_bytes(), *bundle, *map, catalog);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->value(), binding->value());
}

// A declared frame role binds as itself only; a default TCP named
// otherwise (`cup_center`, or a tool's `flange`) has no `tcp` name.
TEST(ProgramBindingCompilerTest, DeclaredFrameRolesBindUnderTheirOwnNameOnly) {
  using manyforge::core::CollisionBodyRoleId;
  using manyforge::core::FrameRoleId;
  using manyforge::core::ModelRoleId;
  const auto frame = [](std::string id, std::vector<CollisionBodyRoleId> bodies,
                        bool tcp) {
    return manyforge::core::ComponentTypeFrameRoleDTO{
        FrameRoleId(id), ModelRoleId("tool_model"), FrameRoleId(id),
        std::move(bodies), tcp};
  };
  manyforge::core::ComponentTypeCommandTargetDTO tool{
      manyforge::core::CommandTargetId("effector"), {}, {}, {}, {}, {}};
  tool.frame_roles = {
      frame("cup_center", {CollisionBodyRoleId("cup_lip")}, true),
      frame("side", {}, false)};
  tool.default_tcp_frame_role_id = FrameRoleId("cup_center");

  namespace detail = manyforge::deployment::detail;
  EXPECT_EQ(detail::declared_target_frame_role(tool, FrameRoleId("tcp")), nullptr);
  const auto *named =
      detail::declared_target_frame_role(tool, FrameRoleId("cup_center"));
  ASSERT_NE(named, nullptr);
  EXPECT_EQ(named->contact_body_role_ids,
            std::vector<CollisionBodyRoleId>{CollisionBodyRoleId("cup_lip")});
  EXPECT_EQ(detail::declared_target_frame_role(tool, FrameRoleId("side")),
            &tool.frame_roles[1]);
  EXPECT_EQ(detail::declared_target_frame_role(tool, FrameRoleId("missing")),
            nullptr);

  // A declared `tcp` binds as itself.
  tool.frame_roles = {frame("tcp", {CollisionBodyRoleId("cup_lip")}, true)};
  tool.default_tcp_frame_role_id = FrameRoleId("tcp");
  EXPECT_EQ(detail::declared_target_frame_role(tool, FrameRoleId("tcp")),
            &tool.frame_roles[0]);

  // A tool's default named `flange` is `flange` only.
  tool.frame_roles = {frame("flange", {CollisionBodyRoleId("cup_lip")}, true)};
  tool.default_tcp_frame_role_id = FrameRoleId("flange");
  EXPECT_EQ(detail::declared_target_frame_role(tool, FrameRoleId("tcp")), nullptr);
  EXPECT_EQ(detail::declared_target_frame_role(tool, FrameRoleId("flange")),
            &tool.frame_roles[0]);
}

// The direct-control TCP list of a robot without a tool lists its
// tagged TCPs only (`base` is never listed), owner-qualified, with the robot's
// own default TCP (its End-effector TCP, `{arm, tcp}`) first. The
// mounted-tool and primary cases run on maintained cells (migration check)
// and in `test_cell_kinematic_index` (the mark itself).
TEST(ProgramBindingCompilerTest, DirectControlMotionTcpsListOnlyTaggedTcps) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_ros_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const manyforge::core::ControllableComponentId component{
      std::string(kLite6ControllableComponentId)};
  const auto direct = compile_direct_control_motion_program_binding(
      component, manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
      *context.bundle, context.model_map, catalog_store);
  ASSERT_NE(direct, nullptr);
  const auto inventory =
      compile_direct_control_program_binding_inventory(
          *context.bundle, context.model_map, catalog_store);
  const auto tcps = manyforge::deployment::direct_control_motion_tcps(
      *direct, inventory, *context.bundle, context.model_map);
  ASSERT_EQ(tcps.size(), 2U);
  const std::string owner(kLite6ControllableComponentId);
  EXPECT_EQ(tcps[0].display, owner + ".tcp");
  EXPECT_TRUE(tcps[0].binding.end_effector);
  EXPECT_EQ(tcps[1].display, owner + ".flange");
  EXPECT_FALSE(tcps[1].binding.end_effector);
  EXPECT_TRUE(tcps[0].mounted_here);
  EXPECT_TRUE(tcps[1].mounted_here);
  EXPECT_FALSE(tcps[0].mounted_elsewhere);
  EXPECT_FALSE(tcps[1].mounted_elsewhere);
  for (const auto &tcp : tcps) {
    EXPECT_EQ(tcp.binding.entry.target.component_id, component);
    EXPECT_EQ(tcp.binding.entry.target.command_target_id.value(), kLite6ArmTargetId);
    EXPECT_EQ(tcp.binding.entry.frame.component_id, component);
  }
  const auto mark = context.model_map.kinematic_index().end_effector_entry(
      component, manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)));
  ASSERT_TRUE(mark.has_value());
  EXPECT_EQ(mark->component_id, component);
  EXPECT_EQ(mark->target_id.value(), kLite6ArmTargetId);
  EXPECT_EQ(mark->frame_role_id.value(), "tcp");
}

TEST(ProgramBindingCompilerTest,
     DirectControlMotionBindingRejectsUnknownComponentOrTarget) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context =
      make_ros_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());

  EXPECT_THROW(
      static_cast<void>(compile_direct_control_motion_program_binding(
          manyforge::core::ControllableComponentId("missing_component"),
          manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
          *context.bundle, context.model_map, catalog_store)),
      ProgramBindingCompilerError);
  EXPECT_THROW(static_cast<void>(compile_direct_control_motion_program_binding(
                   manyforge::core::ControllableComponentId(
                       std::string(kLite6ControllableComponentId)),
                   manyforge::core::CommandTargetId("missing_target"),
                   *context.bundle, context.model_map, catalog_store)),
               ProgramBindingCompilerError);
}

TEST(ProgramBindingCompilerTest,
     DerivesDeterministicDirectControlGripperBindingWithExactSchema) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path(), true);
  FilesystemCatalogArtifactStore catalog_store(root.path());

  const auto first = compile_direct_control_gripper_program_binding(
      manyforge::core::ControllableComponentId(
          std::string(kLiteGripperComponentId)),
      manyforge::core::CommandTargetId(std::string(kLite6GripperTargetId)),
      *context.bundle, context.model_map, catalog_store);
  const auto second = compile_direct_control_gripper_program_binding(
      manyforge::core::ControllableComponentId(
          std::string(kLiteGripperComponentId)),
      manyforge::core::CommandTargetId(std::string(kLite6GripperTargetId)),
      *context.bundle, context.model_map, catalog_store);

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->program_binding_id(), second->program_binding_id());
  EXPECT_EQ(first->canonical_bytes(), second->canonical_bytes());
  EXPECT_EQ(first->value().program_id.value(),
            manyforge::deployment::kDirectControlGripperProgramId);
  EXPECT_TRUE(manyforge::deployment::is_sha256_digest(
      first->value().program_revision.value()));
  ASSERT_EQ(first->value().target_bindings.size(), 1U);
  const auto &target = first->value().target_bindings.front();
  EXPECT_EQ(target.target.component_id.value(), kLiteGripperComponentId);
  EXPECT_EQ(target.target.command_target_id.value(), kLite6GripperTargetId);
  EXPECT_FALSE(target.robot_tcps.has_value());
  EXPECT_EQ(first->canonical_bytes().find("\"robot_tcps\""), std::string::npos);
  const auto reloaded = manyforge::deployment::reload_verified_program_binding(
      first->canonical_bytes(), *context.bundle, context.model_map, catalog_store);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_FALSE(reloaded->value().target_bindings.front().robot_tcps);
  ASSERT_EQ(target.capability_bindings.size(), 1U);
  const auto &capability = target.capability_bindings.front();
  EXPECT_EQ(capability.capability_id.value(), "tool.gripper.command");
  ASSERT_TRUE(capability.command_schema.has_value());
  EXPECT_EQ(capability.command_schema->command_schema_id.value(),
            manyforge::core::kGripperPositionCommandSchemaId);
  EXPECT_EQ(capability.command_schema->command_schema_revision.value(),
            manyforge::core::kGripperPositionCommandSchemaRevision);
  ASSERT_EQ(target.resource_role_bindings.size(), 1U);
  ASSERT_EQ(target.resource_role_bindings.front().resource_ids.size(), 1U);
  EXPECT_TRUE(first->value().contact_grasp_bindings.empty());
}

TEST(ProgramBindingCompilerTest,
     DirectControlGripperBindingRejectsANonGripperTarget) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());

  EXPECT_THROW(
      static_cast<void>(compile_direct_control_gripper_program_binding(
          manyforge::core::ControllableComponentId(
              std::string(kLite6ControllableComponentId)),
          manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
          *context.bundle, context.model_map, catalog_store)),
      ProgramBindingCompilerError);
}

TEST(ProgramBindingCompilerTest,
     DiscoversDirectControlTargetsWithoutAnAuthoredProgram) {
  ScopedTrustedRoot motion_root;
  const BindingCompilerContext motion_context =
      make_ros_motion_context(motion_root.path());
  FilesystemCatalogArtifactStore motion_catalog_store(motion_root.path());
  const auto first_motion = compile_direct_control_program_binding_inventory(
      *motion_context.bundle, motion_context.model_map, motion_catalog_store);
  const auto second_motion = compile_direct_control_program_binding_inventory(
      *motion_context.bundle, motion_context.model_map, motion_catalog_store);

  ASSERT_EQ(first_motion.motion.size(), 1U);
  EXPECT_TRUE(first_motion.gripper.empty());
  ASSERT_EQ(second_motion.motion.size(), 1U);
  EXPECT_EQ(first_motion.motion.front().binding->canonical_bytes(),
            second_motion.motion.front().binding->canonical_bytes());

  ScopedTrustedRoot gripper_root;
  const BindingCompilerContext gripper_context =
      make_context(gripper_root.path(), true);
  FilesystemCatalogArtifactStore gripper_catalog_store(gripper_root.path());
  const auto first_gripper = compile_direct_control_program_binding_inventory(
      *gripper_context.bundle, gripper_context.model_map,
      gripper_catalog_store);
  const auto second_gripper = compile_direct_control_program_binding_inventory(
      *gripper_context.bundle, gripper_context.model_map,
      gripper_catalog_store);

  EXPECT_TRUE(first_gripper.motion.empty());
  ASSERT_EQ(first_gripper.gripper.size(), 1U);
  ASSERT_EQ(second_gripper.gripper.size(), 1U);
  EXPECT_EQ(first_gripper.gripper.front().binding->canonical_bytes(),
            second_gripper.gripper.front().binding->canonical_bytes());
}

TEST(ProgramBindingCompilerTest, RejectsLegacyProgramBeforeBinding) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  try {
    (void)verify_program_binding_contract(make_program_bytes(*context.bundle), *context.bundle);
    FAIL() << "v2 role-based Program must be refused before binding";
  } catch (const manyforge::deployment::ProgramContractError &error) {
    EXPECT_EQ(error.code(), "program_format_unsupported");
    ASSERT_TRUE(error.unsupported_format());
    EXPECT_EQ(error.unsupported_format()->actual_api_version, "manyforge.program.v2");
    EXPECT_EQ(error.unsupported_format()->supported_api_version, "manyforge.program.v3");
  }
}

TEST(ProgramBindingCompilerTest,
     OutcomeBindsExactlyLikeTheCompilerWhenEveryTargetResolves) {
  ScopedTrustedRoot root;
  const auto interrupts = manyforge::deployment::parse_strict_json_model_yaml(
      R"({"interrupt_rules":[]})").root();
  const auto product = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path(), interrupts, "examples/mock_lite6_pneumatic_pick_and_place.scenario");
  ASSERT_TRUE(product.model_map);
  const auto fixture = manyforge::deployment::test::compile_pneumatic_program_fixture(
      root.path(), product);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto outcome = compile_program_binding_outcome(
      *fixture.program, *product.bundle, *product.model_map, catalog_store);
  ASSERT_NE(outcome.binding, nullptr);
  EXPECT_FALSE(outcome.non_executable.has_value());
  EXPECT_FALSE(outcome.resolved_references.has_value());
  EXPECT_EQ(outcome.binding->canonical_bytes(), fixture.binding->canonical_bytes());
}

TEST(ProgramBindingCompilerTest,
     OutcomeRejectsMissingModelMapForNonemptyCandidate) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  manyforge::deployment::test::ProgramOptions options;
  options.catalog_content_version = context.bundle->value().catalogs.node_environment.content_version;
  const auto root_program = manyforge::deployment::test::make_program_root(options);
  const auto authored = manyforge::deployment::test::with_program_revision(
      manyforge::deployment::normalize_program_authoring(root_program));
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto contract = verify_program_binding_contract(canonical_json(authored), *context.bundle, context.model_map, catalog_store);
  EXPECT_THROW(static_cast<void>(compile_program_binding_outcome(
                   *contract, *context.bundle,
                   std::optional<VerifiedRetainedModelMap>{}, catalog_store)),
               ProgramBindingCompilerError);
}

TEST(ProgramBindingCompilerTest, DirectCapabilityProbeUsesTheBindingResolver) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto* descriptor = manyforge::core::find_builtin_capability_definition("state.observed.current");
  ASSERT_NE(descriptor, nullptr);
  const manyforge::core::ProgramCapabilityRequirementDTO capability{
      CapabilityId("state.observed.current"),
      manyforge::core::CapabilityDefinitionRevision(
          std::string(descriptor->capability_definition_revision)),
      manyforge::core::ProgramCapabilityDisposition::kRequired};
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const manyforge::core::CommandTargetId target{std::string(kLite6ArmTargetId)};
  const auto probe = [&](const auto& selected_component, const auto& selected_target) {
    return manyforge::deployment::probe_program_target_capability(
        selected_component, selected_target, capability, *context.bundle, context.model_map,
        catalog_store);
  };
  EXPECT_FALSE(manyforge::deployment::probe_program_target(
                   component, target, *context.bundle, catalog_store).has_value());
  EXPECT_EQ(manyforge::deployment::probe_program_target(
                component, manyforge::core::CommandTargetId("missing"),
                *context.bundle, catalog_store),
            ProgramBindingUnresolvedReason::kTargetMissing);
  EXPECT_FALSE(probe(component, target).has_value());
  EXPECT_EQ(probe(manyforge::core::ControllableComponentId("missing"), target),
            ProgramBindingUnresolvedReason::kComponentMissing);
  EXPECT_EQ(probe(component, manyforge::core::CommandTargetId("missing")),
            ProgramBindingUnresolvedReason::kTargetMissing);
  auto missing_capability = capability;
  missing_capability.capability_id = CapabilityId("missing.capability");
  EXPECT_EQ(manyforge::deployment::probe_program_target_capability(
                component, target, missing_capability, *context.bundle,
                context.model_map, catalog_store),
            ProgramBindingUnresolvedReason::kCapabilityUnavailable);
}

TEST(ProgramBindingCompilerTest, ReferenceCatalogRequiresRetainedIdentity) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  auto environment = context.bundle->value().catalogs.node_environment;
  const auto catalog =
      manyforge::deployment::qualified_program_catalog(environment);
  EXPECT_TRUE(catalog.schema_types.is_object());
  EXPECT_FALSE(catalog.entries.empty());
  environment.catalog_id = "other_catalog";
  EXPECT_THROW(static_cast<void>(
                   manyforge::deployment::qualified_program_catalog(environment)),
               manyforge::deployment::ProgramContractError);
}

TEST(ProgramBindingCompilerTest, MissingCandidateTargetIsRefusedBeforeCapture) {
  ScopedTrustedRoot root;
  const auto interrupts = manyforge::deployment::parse_strict_json_model_yaml(
      R"({"interrupt_rules":[]})").root();
  const auto product = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path(), interrupts, "examples/mock_lite6_pneumatic_pick_and_place.scenario");
  try {
    (void)manyforge::deployment::test::make_pneumatic_program_contract(
        product, root.path(), 1U, 0U, "removed_target");
    FAIL() << "missing direct target must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    ASSERT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &row) { return row.code == "command_target_missing" &&
                                     row.location.node_instance_id == "close_0"; }));
  }
}

TEST(ProgramBindingCompilerTest, TcpFrameNotCarriedBySelectedTargetIsRefusedBeforeCapture) {
  ScopedTrustedRoot root;
  const auto interrupts = manyforge::deployment::parse_strict_json_model_yaml(
      R"({"interrupt_rules":[]})").root();
  const auto product = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path(), interrupts, "examples/mock_lite6_pneumatic_pick_and_place.scenario");
  manyforge::deployment::test::ProgramOptions options;
  options.catalog_content_version = product.bundle->value().catalogs.node_environment.content_version;
  auto program = manyforge::deployment::test::make_program_root(options);
  const auto node = manyforge::deployment::parse_strict_json_model_yaml(R"({
    "node_id":"motion.set_tcp","instance_id":"set_tcp","children":[],
    "parameters":{
      "target":{"component_id":"lite6_robot","command_target_id":"arm"},
      "tcp":{"kind":"entry",
        "target":{"component_id":"lite_gripper","command_target_id":"gripper"},
        "frame":{"component_id":"lite6_robot","frame_id":"base"}}
    }})").root();
  auto fields = program.as_object();
  for (auto &[name, value] : fields)
    if (name == "tree") value = node;
  program = CanonicalValue(std::move(fields));
  program = manyforge::deployment::test::with_program_revision(
      manyforge::deployment::normalize_program_authoring(program));
  FilesystemCatalogArtifactStore catalog(root.path());
  try {
    (void)verify_program_binding_contract(canonical_json(program), *product.bundle,
                                          product.model_map, catalog);
    FAIL() << "a target must not select a frame it does not carry";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    std::string codes;
    for (const auto &row : error.diagnostics())
      codes += row.code + "@" + row.location.node_instance_id.value_or("") + " ";
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &row) { return row.code == "frame_not_mounted" &&
                                     row.location.node_instance_id == "set_tcp"; }))
        << "diagnostics: " << codes;
  }
}

TEST(ProgramBindingCompilerTest, MaintainedSignalAndProbeUseBuiltinInterruptCapabilities) {
  ScopedTrustedRoot root;
  constexpr std::string_view scenario = "examples/mock_lite6_signal_moves.scenario";
  const auto product = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path(), CanonicalValue(CanonicalValue::Object{}), std::filesystem::path(scenario));
  const auto program = manyforge::deployment::parse_strict_json_model_yaml(
      manyforge::deployment::test::maintained_activation_fixture_detail::Read(
          std::filesystem::path(MANYFORGE_WORKSPACE_ROOT) / std::filesystem::path(scenario) / "program.yaml")).root();
  FilesystemCatalogArtifactStore catalog(root.path());
  const auto diagnostics = manyforge::deployment::validate_program_references(
      program, false, *product.bundle, product.model_map, catalog);
  for (const auto &diagnostic : diagnostics) {
    EXPECT_NE(diagnostic.code, "cell_capability_missing")
        << diagnostic.location.path << " " << canonical_json(diagnostic.payload);
  }
}

TEST(ProgramBindingCompilerTest, SimulatorStepRequiresSealedIsaacDependency) {
  const auto interrupts = manyforge::deployment::parse_strict_json_model_yaml(
      R"({"interrupt_rules":[]})").root();
  const auto node = manyforge::deployment::parse_strict_json_model_yaml(R"({
    "node_id":"simulation.step","instance_id":"step","children":[],
    "parameters":{"steps":1}})").root();
  const auto make_bytes = [&](const auto &product) {
    manyforge::deployment::test::ProgramOptions options;
    options.catalog_content_version = product.bundle->value().catalogs.node_environment.content_version;
    auto fields = manyforge::deployment::test::make_program_root(options).as_object();
    for (auto &[name, value] : fields)
      if (name == "tree") value = node;
    return canonical_json(manyforge::deployment::test::with_program_revision(
        manyforge::deployment::normalize_program_authoring(CanonicalValue(std::move(fields)))));
  };
  ScopedTrustedRoot isaac_root;
  const auto isaac = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      isaac_root.path(), interrupts, "examples/sim_lite6_pick_and_place.scenario");
  const auto count_capability = [](const auto &product, std::string_view id) {
    const auto &providers = product.bundle->value().capability_provider_bindings;
    return std::count_if(providers.begin(), providers.end(), [&](const auto &provider) {
      return provider.capability_id.value() == id;
    });
  };
  for (const std::string_view id : {"simulator.control.pause", "simulator.control.play",
                                    "simulator.control.reset", "simulator.control.step",
                                    "simulator.entity_state.set"})
    EXPECT_EQ(count_capability(isaac, id), 1) << id;
  FilesystemCatalogArtifactStore isaac_catalog(isaac_root.path());
  EXPECT_NE(verify_program_binding_contract(make_bytes(isaac), *isaac.bundle,
                                             isaac.model_map, isaac_catalog), nullptr);

  ScopedTrustedRoot mock_root;
  const auto mock = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      mock_root.path(), interrupts, "examples/mock_lite6_pick_and_place.scenario");
  for (const std::string_view id : {"simulator.control.pause", "simulator.control.play",
                                    "simulator.control.reset", "simulator.control.step",
                                    "simulator.entity_state.set"})
    EXPECT_EQ(count_capability(mock, id), 0) << id;
  FilesystemCatalogArtifactStore mock_catalog(mock_root.path());
  try {
    (void)verify_program_binding_contract(make_bytes(mock), *mock.bundle,
                                          mock.model_map, mock_catalog);
    FAIL() << "Mock-only cell must not offer simulator control";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &row) { return row.code == "cell_capability_missing" &&
                                     row.location.node_instance_id == "step"; }));
  }
}

TEST(ProgramBindingCompilerTest, MissingComponentIsRefusedBeforeCapture) {
  ScopedTrustedRoot root;
  const auto interrupts = manyforge::deployment::parse_strict_json_model_yaml(
      R"({"interrupt_rules":[]})").root();
  const auto product = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path(), interrupts, "examples/mock_lite6_pneumatic_pick_and_place.scenario");
  try {
    (void)manyforge::deployment::test::make_pneumatic_program_contract(
        product, root.path(), 1U, 0U, "gripper", "removed_tool");
    FAIL() << "missing direct component must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    ASSERT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &row) { return row.code == "component_reference_missing" &&
                                     row.location.node_instance_id == "close_0"; }));
  }
}

TEST(ProgramBindingCompilerTest,
     MalformedDirectTargetAbortsInsteadOfBecomingNonExecutable) {
  ScopedTrustedRoot root;
  const auto interrupts = manyforge::deployment::parse_strict_json_model_yaml(
      R"({"interrupt_rules":[]})").root();
  const auto product = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path(), interrupts, "examples/mock_lite6_pneumatic_pick_and_place.scenario");
  try {
    (void)manyforge::deployment::test::make_pneumatic_program_contract(product, root.path(), 1U, 0U, "");
    FAIL() << "an empty direct target must be refused";
  } catch (const manyforge::deployment::ProgramContractError &error) {
    EXPECT_EQ(std::string(error.what()).find("reference-use collection is not yet available"),
              std::string::npos);
  }
}

// -- Prepared-activation outcome readback ------------------------------

[[nodiscard]] manyforge::deployment::detail::PreparedActivationMaterialization
make_pneumatic_product(const std::filesystem::path &root) {
  const auto interrupts = manyforge::deployment::parse_strict_json_model_yaml(
      R"({"interrupt_rules":[]})").root();
  return manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root, interrupts, "examples/mock_lite6_pneumatic_pick_and_place.scenario");
}

[[nodiscard]] manyforge::deployment::PreparedActivationInput
prepare_v3(const std::filesystem::path &root,
           const manyforge::deployment::detail::PreparedActivationMaterialization &product,
           std::string_view command_target = "gripper",
           std::string project_path = "/fixtures/scenario.readback.scenario") {
  if (!product.model_map) throw std::logic_error("pneumatic fixture requires retained model map");
  const auto program = manyforge::deployment::test::make_pneumatic_program_contract(
      product, root, 1U, 0U, command_target);
  FilesystemCatalogArtifactStore catalogs(root);
  manyforge::deployment::FilesystemRawArtifactStore raw(root);
  return manyforge::deployment::assemble_prepared_activation(
      {manyforge::deployment::PreparedScenarioOrigin{
           "scenario.readback", manyforge::core::ContentDigest("sha256:" + std::string(64U, '1')),
           std::move(project_path)},
       manyforge::core::ContentDigest("sha256:" + std::string(64U, '2')), "optional"},
      *product.bundle, *product.model_map, *program, catalogs, raw);
}

[[nodiscard]] CanonicalValue with_member(const CanonicalValue &object,
                                         std::string_view name,
                                         CanonicalValue value) {
  CanonicalValue::Object members = object.as_object();
  for (auto &[member, current] : members) {
    if (member == name) {
      current = std::move(value);
      return CanonicalValue(std::move(members));
    }
  }
  throw std::logic_error("test member is absent");
}

/// Re-derive the content identity so identity and structural parsing pass:
/// only recompilation can then detect the forged outcome.
[[nodiscard]] CanonicalValue reidentify(const CanonicalValue &root) {
  CanonicalValue::Object identity;
  for (const auto &[name, value] : root.as_object()) {
    if (name != "prepared_activation_id") identity.emplace_back(name, value);
  }
  const auto id = manyforge::deployment::sha256_digest(
      canonical_json(CanonicalValue(identity)));
  identity.emplace_back("prepared_activation_id", CanonicalValue(id));
  return CanonicalValue(std::move(identity));
}

TEST(ProgramBindingCompilerTest, ReadbackRejectsForgedNonExecutableOutcome) {
  ScopedTrustedRoot root;
  const auto product = make_pneumatic_product(root.path());
  const auto honest = prepare_v3(root.path(), product);
  ASSERT_TRUE(honest.program_binding_id());
  EXPECT_NO_THROW(manyforge::deployment::verify_prepared_program_outcome(honest));
  const auto program = manyforge::deployment::test::make_pneumatic_program_contract(product, root.path());
  // This is untrusted outcome data, not an unvalidated Program contract.
  const manyforge::deployment::ProgramNonExecutableOutcomeDTO claim{
      program->value().program_id, program->value().program_revision,
      product.bundle->retained_bundle_id(), product.bundle->value().cell_graph.graph_revision,
      {{{manyforge::core::ControllableComponentId("lite_gripper"),
         manyforge::core::CommandTargetId("gripper")},
        ProgramBindingUnresolvedReason::kTargetIncompatible, "claimed incompatible target"}},
      {manyforge::core::ProgramNodeInstanceId("close_0")}, {}};
  const auto value = manyforge::deployment::parse_strict_json_model_yaml(
      EncodeProgramNonExecutableOutcome(claim)).root();
  const auto payload = with_member(
      *honest.normalized_root().find_object_member("activation_payload"),
      "program_binding", CanonicalValue(nullptr));
  const auto forged_root = with_member(
      with_member(honest.normalized_root(), "activation_payload", payload),
      "program_outcome", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("non_executable")}, {"outcome", value}}));
  const auto forged = manyforge::deployment::parse_prepared_activation_input(reidentify(forged_root));
  EXPECT_THROW(manyforge::deployment::verify_prepared_program_outcome(forged),
               manyforge::deployment::PreparedActivationInputError);
  ScopedTrustedRoot store;
  const manyforge::deployment::FilesystemPreparedActivationRepository repository(store.path());
  ASSERT_TRUE(repository.put_if_absent(honest));
  ASSERT_TRUE(repository.put_if_absent(forged));
  EXPECT_NE(repository.get_verified(honest.prepared_activation_id()), nullptr);
  EXPECT_THROW(static_cast<void>(repository.get_verified(forged.prepared_activation_id())),
               manyforge::deployment::PreparedActivationInputError);
}

TEST(ProgramBindingCompilerTest,
     PreparedActivationStoreKeepsOnlyTheMostRecentlyWrittenValues) {
  namespace d = manyforge::deployment;
  ScopedTrustedRoot root;
  const auto product = make_pneumatic_product(root.path());
  ScopedTrustedRoot store;
  const d::FilesystemPreparedActivationRepository repository(store.path());
  const auto stored_count = [&] {
    std::size_t count = 0U;
    for (const auto &entry : std::filesystem::directory_iterator(
             store.path() / "prepared-activations"))
      count += entry.path().extension() == ".json" ? 1U : 0U;
    return count;
  };
  constexpr std::size_t kExtra = 4U;
  std::vector<d::PreparedActivationInput> values;
  for (std::size_t index = 0U;
       index < d::kMaximumRetainedPreparedActivations + kExtra; ++index) {
    values.push_back(prepare_v3(root.path(), product, "gripper",
                             "/fixtures/scenario-" + std::to_string(index) +
                                 ".readback.scenario"));
    ASSERT_TRUE(repository.put_if_absent(values.back()));
    EXPECT_LE(stored_count(), d::kMaximumRetainedPreparedActivations);
  }
  EXPECT_EQ(stored_count(), d::kMaximumRetainedPreparedActivations);
  for (std::size_t index = 0U; index < values.size(); ++index)
    EXPECT_EQ(repository.get_verified(values[index].prepared_activation_id()) !=
                  nullptr,
              index >= kExtra)
        << index;

  // Re-adopting the oldest retained value makes it the most recent: the next
  // write removes the one after it instead.
  EXPECT_FALSE(repository.put_if_absent(values[kExtra]));
  ASSERT_TRUE(repository.put_if_absent(prepare_v3(
      root.path(), product, "gripper", "/fixtures/scenario-new.readback.scenario")));
  EXPECT_EQ(stored_count(), d::kMaximumRetainedPreparedActivations);
  EXPECT_NE(repository.get_verified(values[kExtra].prepared_activation_id()),
            nullptr);
  EXPECT_EQ(repository.get_verified(values[kExtra + 1U].prepared_activation_id()),
            nullptr);
}

TEST(ProgramBindingCompilerTest, NoProgramReadbackProvesAbsenceWithoutInventingABinding) {
  ScopedTrustedRoot root;
  const auto interrupts = manyforge::deployment::parse_strict_json_model_yaml(
      R"({"interrupt_rules":[]})").root();
  const auto product = manyforge::deployment::test::MaterializeMaintainedInterruptDeployment(
      root.path(), interrupts, "examples/mock_lite6_pneumatic_pick_and_place.scenario");
  ASSERT_TRUE(product.model_map);
  const auto program = manyforge::deployment::test::make_pneumatic_program_contract(product, root.path());
  FilesystemCatalogArtifactStore catalogs(root.path());
  manyforge::deployment::FilesystemRawArtifactStore raw(root.path());
  const auto bound = manyforge::deployment::assemble_prepared_activation(
      {manyforge::deployment::PreparedScenarioOrigin{
           "scenario.readback", manyforge::core::ContentDigest("sha256:" + std::string(64U, '1')),
           "/fixtures/scenario.readback.scenario"},
       manyforge::core::ContentDigest("sha256:" + std::string(64U, '2')), "optional"},
      *product.bundle, *product.model_map, *program, catalogs, raw);
  auto payload = *bound.normalized_root().find_object_member("activation_payload");
  payload = with_member(payload, "program", CanonicalValue(nullptr));
  payload = with_member(payload, "program_binding", CanonicalValue(nullptr));
  auto value = with_member(bound.normalized_root(), "activation_payload", payload);
  value = with_member(value, "program_id", CanonicalValue(nullptr));
  value = with_member(value, "program_revision", CanonicalValue(nullptr));
  value = with_member(value, "program_outcome", CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("no_program")},
  }));
  const auto prepared = manyforge::deployment::parse_prepared_activation_input(reidentify(value));
  EXPECT_EQ(prepared.closure().program, nullptr);
  EXPECT_EQ(prepared.closure().program_binding, nullptr);
  EXPECT_FALSE(prepared.program_id().has_value());
  EXPECT_FALSE(prepared.program_revision().has_value());
  EXPECT_FALSE(prepared.program_outcome().has_value());
  EXPECT_NO_THROW(manyforge::deployment::verify_prepared_program_outcome(prepared));
  const manyforge::deployment::FilesystemPreparedActivationRepository repository(root.path());
  ASSERT_TRUE(repository.put_if_absent(prepared));
  EXPECT_NE(repository.get_verified(prepared.prepared_activation_id()), nullptr);

  // Rehash every forgery: rejection must follow semantic inconsistency,
  // not merely a stale envelope digest.
  for (const auto field : {"program", "program_binding"}) {
    const auto forged_payload = with_member(payload, field,
        *bound.normalized_root().find_object_member("activation_payload")->find_object_member(field));
    EXPECT_THROW(static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
        reidentify(with_member(value, "activation_payload", forged_payload)))),
        manyforge::deployment::PreparedActivationInputError);
  }
  // A removed role-assignment field cannot be smuggled into a current
  // no-Program payload, even with a freshly computed envelope identity.
  auto legacy_fields = payload.as_object();
  legacy_fields.emplace_back("program_role_assignment", CanonicalValue(CanonicalValue::Object{
      {"component_roles", CanonicalValue(CanonicalValue::Array{})},
      {"target_roles", CanonicalValue(CanonicalValue::Array{})}}));
  const CanonicalValue legacy_payload(std::move(legacy_fields));
  EXPECT_THROW(static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
      reidentify(with_member(value, "activation_payload", legacy_payload)))),
      manyforge::deployment::PreparedActivationInputError);
  for (const auto field : {"program_id", "program_revision"}) {
    EXPECT_THROW(static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
        reidentify(with_member(value, field, *bound.normalized_root().find_object_member(field))))),
        manyforge::deployment::PreparedActivationInputError);
  }
  EXPECT_THROW(static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
      reidentify(with_member(value, "schema_version", CanonicalValue("manyforge.prepared_activation_input.v3"))))),
      manyforge::deployment::PreparedActivationInputError);
}

TEST(ProgramBindingCompilerTest, AssemblesANoProgramActivationWithoutAProgramClosure) {
  ScopedTrustedRoot root;
  const auto product = make_pneumatic_product(root.path());
  ASSERT_TRUE(product.model_map);
  manyforge::deployment::FilesystemRawArtifactStore raw(root.path());
  const manyforge::deployment::PreparedActivationAssemblyInput input{
      manyforge::deployment::PreparedScenarioOrigin{
          "scenario.close", manyforge::core::ContentDigest("sha256:" + std::string(64U, '1')),
          "/fixtures/scenario.close.scenario"},
      manyforge::core::ContentDigest("sha256:" + std::string(64U, '2')), "optional"};
  const auto prepared = manyforge::deployment::assemble_prepared_activation_without_program(
      input, *product.bundle, product.model_map, raw);
  EXPECT_EQ(prepared.retained_bundle_id(), product.bundle->retained_bundle_id());
  EXPECT_EQ(prepared.closure().program, nullptr);
  EXPECT_EQ(prepared.closure().program_binding, nullptr);
  EXPECT_TRUE(prepared.closure().catalog_artifacts.empty());
  EXPECT_TRUE(prepared.closure().retained_model_map.has_value());
  EXPECT_FALSE(prepared.program_id().has_value());
  EXPECT_FALSE(prepared.program_revision().has_value());
  EXPECT_FALSE(prepared.program_binding_id().has_value());
  EXPECT_FALSE(prepared.program_outcome().has_value());
  EXPECT_EQ(*prepared.normalized_root().find_object_member("program_outcome"),
            CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue("no_program")}}));
  EXPECT_NO_THROW(manyforge::deployment::verify_prepared_program_outcome(prepared));
  const manyforge::deployment::FilesystemPreparedActivationRepository repository(root.path());
  ASSERT_TRUE(repository.put_if_absent(prepared));
  EXPECT_NE(repository.get_verified(prepared.prepared_activation_id()), nullptr);

  // no_program admits only the optional assistant mode.
  auto required = input;
  required.assistant_mode = "required";
  EXPECT_THROW(static_cast<void>(
                   manyforge::deployment::assemble_prepared_activation_without_program(
                       required, *product.bundle, product.model_map, raw)),
               manyforge::deployment::PreparedActivationInputError);
}

TEST(ProgramBindingCompilerTest,
     ReadbackRefusesTheRemovedParameterOverrideForm) {
  ScopedTrustedRoot root;
  const auto product = make_pneumatic_product(root.path());
  const auto bound = prepare_v3(root.path(), product, "gripper");
  // v4 carried launch-time typed_parameter_overrides. Even rehashed it is
  // refused by version with guidance, never read as an override-free value.
  auto removed = bound.normalized_root().as_object();
  removed.emplace_back("typed_parameter_overrides",
                       CanonicalValue(CanonicalValue::Object{}));
  try {
    static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
        reidentify(with_member(CanonicalValue(std::move(removed)), "schema_version",
                               CanonicalValue("manyforge.prepared_activation_input.v4")))));
    FAIL() << "the removed v4 prepared activation form was accepted";
  } catch (const manyforge::deployment::PreparedActivationInputError &error) {
    EXPECT_NE(std::string(error.what()).find("parameter overrides were removed"),
              std::string::npos);
  }
  // The current version cannot smuggle the removed field either.
  auto smuggled = bound.normalized_root().as_object();
  smuggled.emplace_back("typed_parameter_overrides",
                        CanonicalValue(CanonicalValue::Object{}));
  EXPECT_THROW(static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
                   reidentify(CanonicalValue(std::move(smuggled))))),
               manyforge::deployment::PreparedActivationInputError);
}

TEST(ProgramBindingCompilerTest,
     ReadbackRejectsANonExecutableOutcomeForgedOverABindableProgram) {
  ScopedTrustedRoot root;
  const auto product = make_pneumatic_product(root.path());
  const auto bound = prepare_v3(root.path(), product, "gripper");
  ASSERT_TRUE(bound.program_binding_id().has_value());
  const auto program = manyforge::deployment::test::make_pneumatic_program_contract(product, root.path());
  // Construct an untrusted claimed outcome, never an unvalidated contract.
  const manyforge::deployment::ProgramNonExecutableOutcomeDTO claim{
      program->value().program_id, program->value().program_revision,
      product.bundle->retained_bundle_id(), product.bundle->value().cell_graph.graph_revision,
      {{ {manyforge::core::ControllableComponentId("lite_gripper"),
           manyforge::core::CommandTargetId("gripper")},
         ProgramBindingUnresolvedReason::kTargetMissing, "claimed missing target" }},
      {manyforge::core::ProgramNodeInstanceId("close_0")}, {}};
  const auto forged_outcome = manyforge::deployment::parse_strict_json_model_yaml(
      EncodeProgramNonExecutableOutcome(claim)).root();

  // Claim "non-executable" (and drop the binding) for a Program whose
  // direct target actually binds.
  auto payload = with_member(
      *bound.normalized_root().find_object_member("activation_payload"),
      "program_binding", CanonicalValue(nullptr));
  const auto forged_root = with_member(
      with_member(bound.normalized_root(), "activation_payload", std::move(payload)),
      "program_outcome", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("non_executable")}, {"outcome", forged_outcome}}));
  const auto forged =
      manyforge::deployment::parse_prepared_activation_input(reidentify(forged_root));
  EXPECT_THROW(manyforge::deployment::verify_prepared_program_outcome(forged),
               manyforge::deployment::PreparedActivationInputError);
}

[[nodiscard]] CanonicalValue scene_box(std::string id, CanonicalValue frame_ref) {
  return CanonicalValue(CanonicalValue::Object{
      {"pose",
       CanonicalValue(CanonicalValue::Object{
           {"frame_ref", std::move(frame_ref)},
           {"orientation",
            CanonicalValue(CanonicalValue::Object{
                {"rpy_deg", CanonicalValue(CanonicalValue::Array{
                                CanonicalValue(0.0), CanonicalValue(0.0),
                                CanonicalValue(0.0)})},
            })},
           {"position_m", CanonicalValue(CanonicalValue::Array{
                              CanonicalValue(0.1), CanonicalValue(0.0),
                              CanonicalValue(0.05)})},
       })},
      {"scene_resource_id", CanonicalValue(std::move(id))},
      {"shape",
       CanonicalValue(CanonicalValue::Object{
           {"box_dimensions_m", CanonicalValue(CanonicalValue::Array{
                                    CanonicalValue(0.02), CanonicalValue(0.02),
                                    CanonicalValue(0.02)})},
           {"type", CanonicalValue("box")},
       })},
  });
}

TEST(ProgramBindingCompilerTest, UnusedAmbiguousFrameDoesNotPreventBinding) {
  ScopedTrustedRoot root;
  const auto materialized = manyforge::deployment::test::materialize_lite6_retained_consumer_fixture_from(
      root.path(), manyforge::deployment::test::make_lite6_ros2_control_observed_consumer_retention_fixture(
                       false, false, "flange", false, false, true));
  const FilesystemVerifiedRetainedBundleSource bundle_source(root.path());
  const auto bundle = bundle_source.get_verified(materialized.bundle->retained_bundle_id());
  ASSERT_NE(bundle, nullptr);
  ASSERT_EQ(bundle->value().compiled_products.symbol_role_maps.size(), 1U);
  const auto &map_product = bundle->value().compiled_products.symbol_role_maps.front();
  const FilesystemVerifiedRetainedModelMapSource map_source(root.path());
  const auto map = map_source.get_verified(RetainedModelMapQuery{bundle->retained_bundle_id(), map_product.product_id});
  ASSERT_TRUE(map);
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const manyforge::core::ProgramFrameReferenceDTO ambiguous{component, manyforge::core::FrameRoleId("root")};
  const manyforge::core::ProgramFrameReferenceDTO base{component, manyforge::core::FrameRoleId("base")};
  ASSERT_EQ(manyforge::deployment::find_program_frame_matches(ambiguous, *map).size(), 2U);
  ASSERT_EQ(manyforge::deployment::find_program_frame_matches(base, *map).size(), 1U);

  manyforge::deployment::test::ProgramOptions options;
  options.catalog_content_version = bundle->value().catalogs.node_environment.content_version;
  auto fields = manyforge::deployment::test::make_program_root(options).as_object();
  fields.emplace_back("scene", CanonicalValue(CanonicalValue::Array{
      scene_box("on_base", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("component_frame")},
          {"frame", CanonicalValue(CanonicalValue::Object{
              {"component_id", CanonicalValue(std::string(kLite6ControllableComponentId))},
              {"frame_id", CanonicalValue("base")}})}}))}));
  const auto authored = manyforge::deployment::test::with_program_revision(
      manyforge::deployment::normalize_program_authoring(CanonicalValue(std::move(fields))));
  FilesystemCatalogArtifactStore catalog(root.path());
  const auto program = verify_program_binding_contract(canonical_json(authored), *bundle, *map, catalog);
  const auto binding = compile_program_binding(*program, *bundle, *map, catalog);
  ASSERT_NE(binding, nullptr);
  const auto &frames = binding->value().frame_bindings;
  EXPECT_NE(std::find_if(frames.begin(), frames.end(), [&](const auto &row) { return row.reference == base; }),
            frames.end());
  EXPECT_EQ(std::find_if(frames.begin(), frames.end(), [&](const auto &row) { return row.reference == ambiguous; }),
            frames.end());
  const auto reloaded = manyforge::deployment::reload_verified_program_binding(
      binding->canonical_bytes(), *bundle, *map, catalog);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->value(), binding->value());

  // Exercise the actual pre-Apply assembly authority, including its shared
  // reference validation, against the same retained candidate.
  manyforge::deployment::FilesystemRawArtifactStore raw(root.path());
  const auto prepared = manyforge::deployment::assemble_prepared_activation(
      {manyforge::deployment::PreparedScenarioOrigin{
           "scenario.ambiguous-unused", manyforge::core::ContentDigest("sha256:" + std::string(64U, '1')),
           "/fixtures/ambiguous-unused.scenario"},
       manyforge::core::ContentDigest("sha256:" + std::string(64U, '2')), "optional"},
      *bundle, *map, *program, catalog, raw);
  EXPECT_TRUE(prepared.program_binding_id().has_value());
}

/// One Scene object follows an exact component frame; another uses the cell.
[[nodiscard]] std::string program_with_scene(const RetainedBundle &bundle,
                                             std::string component) {
  manyforge::deployment::test::ProgramOptions options;
  options.catalog_content_version = bundle.value().catalogs.node_environment.content_version;
  auto program = manyforge::deployment::test::make_program_root(options);
  const auto target = CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(component)}, {"command_target_id", CanonicalValue("arm")}});
  const auto frame = CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("component_frame")},
      {"frame", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(component)}, {"frame_id", CanonicalValue("tcp")}})}});
  auto fields = program.as_object();
  fields.emplace_back("scene", CanonicalValue(CanonicalValue::Array{
      scene_box("on_arm_tcp", frame),
      scene_box("table_marker", CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue("cell_frame")}}))}));
  program = CanonicalValue(std::move(fields));
  program = with_member(program, "tree", CanonicalValue(CanonicalValue::Object{
      {"node_id", CanonicalValue("gripper.command_position")}, {"instance_id", CanonicalValue("close")},
      {"parameters", CanonicalValue(CanonicalValue::Object{
          {"target", target}, {"position", CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("named_preset")}, {"name", CanonicalValue("closed")}})}})},
      {"children", CanonicalValue(CanonicalValue::Array{})}}));
  return canonical_json(manyforge::deployment::test::with_program_revision(
      manyforge::deployment::normalize_program_authoring(program)));
}

TEST(ProgramBindingCompilerTest, MissingSceneComponentIsRefusedBeforeCapture) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  FilesystemCatalogArtifactStore catalog(root.path());
  try {
    (void)verify_program_binding_contract(
        program_with_scene(*context.bundle, "removed_robot"), *context.bundle, context.model_map, catalog);
    FAIL() << "missing Scene component must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &row) { return row.code == "component_reference_missing"; }));
  }
}

TEST(ProgramBindingCompilerTest, MissingTargetCapabilityIsRefusedBeforeCapture) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path());
  FilesystemCatalogArtifactStore catalog(root.path());
  try {
    (void)verify_program_binding_contract(
        program_with_scene(*context.bundle, std::string(kLite6ControllableComponentId)),
        *context.bundle, context.model_map, catalog);
    FAIL() << "unavailable target capability must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &row) { return row.code == "capability_missing"; }));
  }
}

TEST(ProgramBindingCompilerTest, UnsortedQualifiedEndpointContactsSealAndReloadInCanonicalOrder) {
  ScopedTrustedRoot root;
  auto fixture = manyforge::deployment::test::
      make_lite6_ros2_control_observed_consumer_retention_fixture(
          true, false, "flange", false, true);
  const auto &qualified_targets = fixture.component_type.value().command_targets;
  ASSERT_EQ(qualified_targets.size(), 1U);
  const auto &frames = qualified_targets.front().frame_roles;
  const auto qualified_tcp = std::find_if(frames.begin(), frames.end(), [](const auto &frame) {
    return frame.frame_role_id.value() == "tcp";
  });
  ASSERT_NE(qualified_tcp, frames.end());
  using manyforge::core::CollisionBodyRoleId;
  // The fixture authors {arm_link1, arm_collision}; the qualified type holds
  // them in canonical (sorted) order, which the binding must then preserve.
  ASSERT_EQ(qualified_tcp->contact_body_role_ids,
            (std::vector<CollisionBodyRoleId>{CollisionBodyRoleId("arm_collision"),
                                              CollisionBodyRoleId("arm_link1")}));

  const auto materialized = manyforge::deployment::test::
      materialize_lite6_retained_consumer_fixture_from(root.path(), std::move(fixture));
  const auto &bundle = *materialized.bundle;
  ASSERT_EQ(bundle.value().compiled_products.symbol_role_maps.size(), 1U);
  FilesystemVerifiedRetainedModelMapSource maps(root.path());
  const auto map = maps.get_verified({bundle.retained_bundle_id(),
      bundle.value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(map.has_value());
  FilesystemCatalogArtifactStore catalog(root.path());
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const auto binding = compile_direct_control_motion_program_binding(
      component, manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
      bundle, *map, catalog);
  ASSERT_NE(binding, nullptr);
  ASSERT_EQ(binding->value().target_bindings.size(), 1U);
  const auto &target = binding->value().target_bindings.front();
  // The separately authored model body is a genuine strict subset of the
  // target's retained collision body, not merely another alias for it.
  const manyforge::core::ProgramCollisionBodyReferenceDTO subset_ref{
      component, CollisionBodyRoleId("arm_link1")};
  const auto subset = manyforge::deployment::find_program_collision_body_matches(subset_ref, *map);
  ASSERT_EQ(subset.size(), 1U);
  const auto *retained_target = map->find_target(component,
      manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)));
  ASSERT_NE(retained_target, nullptr);
  ASSERT_FALSE(subset.front().compiled_collision_body_names.empty());
  EXPECT_TRUE(std::any_of(retained_target->collision_bodies.begin(),
      retained_target->collision_bodies.end(), [&](const auto &body) {
        return body.model_instance_id == subset.front().model_instance_id &&
            body.compiled_link_names.size() > subset.front().compiled_collision_body_names.size() &&
            std::all_of(subset.front().compiled_collision_body_names.begin(),
                subset.front().compiled_collision_body_names.end(), [&](const auto &name) {
                  return std::find(body.compiled_link_names.begin(), body.compiled_link_names.end(),
                                   name) != body.compiled_link_names.end();
                });
      }));
  EXPECT_NE(std::find(target.collision_body_references.begin(),
                      target.collision_body_references.end(), subset_ref),
            target.collision_body_references.end());
  const auto association = std::find_if(target.frame_associations.begin(), target.frame_associations.end(),
      [](const auto &row) { return row.frame.frame_id.value() == "tcp"; });
  ASSERT_NE(association, target.frame_associations.end());
  const std::vector<CollisionBodyRoleId> sorted{CollisionBodyRoleId("arm_collision"),
                                                CollisionBodyRoleId("arm_link1")};
  EXPECT_EQ(association->contact_body_role_ids, sorted);
  ASSERT_TRUE(target.robot_tcps.has_value());
  const auto tcp = std::find_if(target.robot_tcps->begin(), target.robot_tcps->end(),
      [](const auto &row) { return row.entry.frame.frame_id.value() == "tcp"; });
  ASSERT_NE(tcp, target.robot_tcps->end());
  EXPECT_EQ(tcp->contact_body_role_ids, sorted);
  for (const auto &role : sorted) {
    const manyforge::core::ProgramCollisionBodyReferenceDTO reference{component, role};
    EXPECT_EQ(std::count_if(binding->value().collision_body_bindings.begin(),
                            binding->value().collision_body_bindings.end(),
                            [&](const auto &row) { return row.reference == reference; }), 1);
  }
  const auto reloaded = manyforge::deployment::reload_verified_program_binding(
      binding->canonical_bytes(), bundle, *map, catalog);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->value(), binding->value());
  EXPECT_EQ(reloaded->canonical_bytes(), binding->canonical_bytes());
}

TEST(ProgramBindingCompilerTest,
     DistinctModeledEndpointsMayShareOneRetainedBodyGroup) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), false, true, true);
  FilesystemCatalogArtifactStore catalog(root.path());
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const manyforge::core::CommandTargetId target_id{std::string(kLite6ArmTargetId)};
  const auto binding = compile_direct_control_motion_program_binding(
      component, target_id, *context.bundle, context.model_map, catalog);
  ASSERT_NE(binding, nullptr);
  ASSERT_EQ(binding->value().target_bindings.size(), 1U);
  const auto &target = binding->value().target_bindings.front();
  const auto find_frame = [&](std::string_view id) {
    return std::find_if(target.frame_associations.begin(), target.frame_associations.end(),
        [&](const auto &row) { return row.frame.component_id == component && row.frame.frame_id.value() == id; });
  };
  const auto first = find_frame("base");
  const auto second = find_frame("tcp");
  ASSERT_NE(first, target.frame_associations.end());
  ASSERT_NE(second, target.frame_associations.end());
  EXPECT_EQ(first->contact_body_role_ids, second->contact_body_role_ids);
  ASSERT_EQ(first->contact_body_role_ids,
            std::vector<manyforge::core::CollisionBodyRoleId>{
                manyforge::core::CollisionBodyRoleId("arm_collision")});
  const auto physical = [&](const auto &reference) {
    return std::find_if(binding->value().frame_bindings.begin(), binding->value().frame_bindings.end(),
        [&](const auto &row) { return row.reference == reference; });
  };
  const auto base = physical(first->frame);
  const auto tcp = physical(second->frame);
  ASSERT_NE(base, binding->value().frame_bindings.end());
  ASSERT_NE(tcp, binding->value().frame_bindings.end());
  EXPECT_NE(base->compiled_frame_name, tcp->compiled_frame_name);
  EXPECT_EQ(base->model_instance_id, tcp->model_instance_id);
  EXPECT_EQ(base->model_revision, tcp->model_revision);
  const manyforge::core::ProgramCollisionBodyReferenceDTO body{component, first->contact_body_role_ids.front()};
  EXPECT_EQ(std::count_if(binding->value().collision_body_bindings.begin(),
                          binding->value().collision_body_bindings.end(),
                          [&](const auto &row) { return row.reference == body; }), 1);
  const auto reloaded = manyforge::deployment::reload_verified_program_binding(
      binding->canonical_bytes(), *context.bundle, context.model_map, catalog);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->value(), binding->value());
  ScopedTrustedRoot missing_catalog_root;
  FilesystemCatalogArtifactStore missing_catalog(missing_catalog_root.path());
  EXPECT_THROW(static_cast<void>(manyforge::deployment::reload_verified_program_binding(
                   binding->canonical_bytes(), *context.bundle, context.model_map, missing_catalog)),
               manyforge::deployment::ProgramBindingError);

  auto altered_association = binding->value();
  auto &associations = altered_association.target_bindings.front().frame_associations;
  const auto association = std::find_if(associations.begin(), associations.end(),
      [](const auto &row) { return !row.contact_body_role_ids.empty(); });
  ASSERT_NE(association, associations.end());
  association->contact_body_role_ids.clear();
  const auto sealed_association = manyforge::deployment::detail::ProgramBindingConstructionAuthority::seal(
      std::move(altered_association));
  EXPECT_THROW(static_cast<void>(manyforge::deployment::reload_verified_program_binding(
                   sealed_association->canonical_bytes(), *context.bundle, context.model_map, catalog)),
               manyforge::deployment::ProgramBindingError);

  auto omitted_tcp = binding->value();
  ASSERT_TRUE(omitted_tcp.target_bindings.front().robot_tcps);
  ASSERT_GT(omitted_tcp.target_bindings.front().robot_tcps->size(), 1U);
  omitted_tcp.target_bindings.front().robot_tcps->pop_back();
  const auto sealed_omission = manyforge::deployment::detail::ProgramBindingConstructionAuthority::seal(
      std::move(omitted_tcp));
  EXPECT_THROW(static_cast<void>(manyforge::deployment::reload_verified_program_binding(
                   sealed_omission->canonical_bytes(), *context.bundle, context.model_map, catalog)),
               manyforge::deployment::ProgramBindingError);

  auto altered_tcp = binding->value();
  ASSERT_TRUE(altered_tcp.target_bindings.front().robot_tcps);
  auto &tcps = *altered_tcp.target_bindings.front().robot_tcps;
  const auto tcp_with_contacts = std::find_if(tcps.begin(), tcps.end(),
      [](const auto &row) { return !row.contact_body_role_ids.empty(); });
  ASSERT_NE(tcp_with_contacts, tcps.end());
  tcp_with_contacts->contact_body_role_ids.clear();
  const auto sealed_tcp = manyforge::deployment::detail::ProgramBindingConstructionAuthority::seal(
      std::move(altered_tcp));
  EXPECT_THROW(static_cast<void>(manyforge::deployment::reload_verified_program_binding(
                   sealed_tcp->canonical_bytes(), *context.bundle, context.model_map, catalog)),
               manyforge::deployment::ProgramBindingError);
}

TEST(ProgramBindingCompilerTest,
     RejectsUnknownRuntimeSelectorsAndCrossBundleContext) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_ros_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const manyforge::core::CommandTargetId target{std::string(kLite6ArmTargetId)};
  const auto binding = compile_direct_control_motion_program_binding(
      component, target, *context.bundle, context.model_map, catalog_store);
  ASSERT_NE(binding, nullptr);
  const manyforge::core::ProgramTargetReferenceDTO exact{component, target};
  const manyforge::core::ProgramTargetReferenceDTO missing{
      component, manyforge::core::CommandTargetId("missing")};
  EXPECT_THROW(static_cast<void>(resolve_runtime_program_target(
                   *binding, missing, CapabilityId("execution.trajectory.execute"),
                   *context.bundle, context.model_map)), RuntimeProgramBindingResolutionError);
  EXPECT_THROW(static_cast<void>(resolve_runtime_program_target(
                   *binding, exact, CapabilityId("missing.capability"),
                   *context.bundle, context.model_map)), RuntimeProgramBindingResolutionError);
  ScopedTrustedRoot other_root;
  const BindingCompilerContext other = make_context(other_root.path());
  EXPECT_THROW(static_cast<void>(resolve_runtime_program_target(
                   *binding, exact, CapabilityId("execution.trajectory.execute"),
                   *other.bundle, other.model_map)), RuntimeProgramBindingResolutionError);
}

TEST(ProgramBindingCompilerTest,
     RejectsMissingRuntimeEpochAndNonDesiredI6Route) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_ros_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const manyforge::core::ControllableComponentId component{std::string(kLite6ControllableComponentId)};
  const manyforge::core::CommandTargetId command_target{std::string(kLite6ArmTargetId)};
  const auto binding = compile_direct_control_motion_program_binding(
      component, command_target, *context.bundle, context.model_map, catalog_store);
  ASSERT_NE(binding, nullptr);
  const auto target = resolve_runtime_program_target(
      *binding, {component, command_target}, CapabilityId("execution.trajectory.execute"),
      *context.bundle, context.model_map);

  auto missing_epoch = active_facts_for(target);
  missing_epoch.provider_epochs.clear();
  EXPECT_THROW(static_cast<void>(build_runtime_participant_binding_dto(
                   target, missing_epoch)),
               RuntimeProgramBindingResolutionError);

  auto mismatched_provider_runtime = active_facts_for(target);
  mismatched_provider_runtime.provider_epochs.front().provider_runtime_unit_id =
      manyforge::core::RuntimeUnitId("runtime.other");
  EXPECT_THROW(static_cast<void>(build_runtime_participant_binding_dto(
                   target, mismatched_provider_runtime)),
               RuntimeProgramBindingResolutionError);

  auto foreign_provider = active_facts_for(target);
  ASSERT_TRUE(target.control_binding().has_value());
  const auto provider_fact = std::find_if(
      foreign_provider.provider_epochs.begin(),
      foreign_provider.provider_epochs.end(), [&](const auto &fact) {
        return fact.provider_instance_id ==
               target.control_binding()->provider_instance_id;
      });
  ASSERT_NE(provider_fact, foreign_provider.provider_epochs.end());
  provider_fact->provider_instance_id =
      manyforge::core::ProviderInstanceId("foreign_provider");
  EXPECT_THROW(static_cast<void>(build_runtime_participant_binding_dto(
                   target, foreign_provider)),
               RuntimeProgramBindingResolutionError);

  auto wrong_route = active_facts_for(target);
  wrong_route.control_route_id = manyforge::core::ControlRouteId("other");
  EXPECT_THROW(static_cast<void>(
                   build_runtime_participant_binding_dto(target, wrong_route)),
               RuntimeProgramBindingResolutionError);
}

std::string output_binding_program(
    const RetainedBundle &bundle, bool mixed = false, std::string_view output_signal = "lamp") {
  manyforge::deployment::test::ProgramOptions options;
  options.catalog_content_version = bundle.value().catalogs.node_environment.content_version;
  auto root = manyforge::deployment::test::make_program_root(options);
  const auto signal = [](std::string_view name) {
    return CanonicalValue(CanonicalValue::Object{
        {"component_id", CanonicalValue("device.io")}, {"signal_name", CanonicalValue(std::string(name))}});
  };
  CanonicalValue::Array children;
  if (mixed)
    children.emplace_back(CanonicalValue::Object{
        {"node_id", CanonicalValue("io.check_input")}, {"instance_id", CanonicalValue("check")},
        {"parameters", CanonicalValue(CanonicalValue::Object{
            {"signal", signal("ready")}, {"expected_value", CanonicalValue(true)}})},
        {"children", CanonicalValue(CanonicalValue::Array{})}});
  children.emplace_back(CanonicalValue::Object{
      {"node_id", CanonicalValue("io.set_output")}, {"instance_id", CanonicalValue("write")},
      {"parameters", CanonicalValue(CanonicalValue::Object{
          {"signal", signal(output_signal)}, {"value", CanonicalValue(true)}})},
      {"children", CanonicalValue(CanonicalValue::Array{})}});
  root = with_member(root, "tree", CanonicalValue(CanonicalValue::Object{
      {"node_id", CanonicalValue("sequence")}, {"instance_id", CanonicalValue("root")},
      {"parameters", CanonicalValue(CanonicalValue::Object{})},
      {"children", CanonicalValue(std::move(children))}}));
  return canonical_json(manyforge::deployment::test::with_program_revision(
      manyforge::deployment::normalize_program_authoring(root)));
}

TEST(ProgramBindingCompilerTest,
     ModelFreeOutputAndMixedSignalsCompileAndReload) {
  for (const bool mixed : {false, true}) {
    ScopedTrustedRoot root;
    const auto fixture =
        manyforge::deployment::test::materialize_boolean_io_retained_fixture(
            root.path(), mixed, true);
    const auto &bundle = *fixture.bundle;
    FilesystemVerifiedRetainedModelMapSource maps(root.path());
    const auto map = maps.get_verified(
        {bundle.retained_bundle_id(),
         bundle.value().compiled_products.symbol_role_maps.front().product_id});
    ASSERT_TRUE(map);
    FilesystemCatalogArtifactStore catalog(root.path());
    const auto program = verify_program_binding_contract(
        output_binding_program(bundle, mixed), bundle, *map, catalog);
    const auto binding =
        compile_program_binding(*program, bundle, *map, catalog);
    ASSERT_NE(binding, nullptr);
    const auto &target = binding->value().target_bindings.front();
    EXPECT_TRUE(target.resource_role_bindings.empty());
    ASSERT_EQ(target.signal_references.size(), mixed ? 2U : 1U);
    const auto output = std::find_if(binding->value().signal_bindings.begin(),
        binding->value().signal_bindings.end(), [](const auto &row) {
          return row.reference.component_id.value() == "device.io" && row.reference.signal_name == "lamp";
        });
    ASSERT_NE(output, binding->value().signal_bindings.end());
    EXPECT_EQ(output->direction, manyforge::core::IoSignalDirection::kOutput);
    EXPECT_NE(std::find(target.signal_references.begin(), target.signal_references.end(),
                        output->reference), target.signal_references.end());
    const auto capability =
        std::find_if(target.capability_bindings.begin(),
                     target.capability_bindings.end(), [](const auto &row) {
                       return row.capability_id.value() == "io.gpio.set_output";
                     });
    ASSERT_NE(capability, target.capability_bindings.end());
    EXPECT_TRUE(capability->capability_provider_binding);
    EXPECT_FALSE(capability->control_binding);
    EXPECT_FALSE(capability->command_schema);
    EXPECT_EQ(reload_verified_program_binding(binding->canonical_bytes(),
                                              bundle, *map, catalog)
                  ->value(),
              binding->value());
    const auto direct =
        compile_direct_control_program_binding_inventory(bundle, *map, catalog);
    ASSERT_EQ(direct.io.size(), 1U);
    EXPECT_TRUE(direct.motion.empty());
    EXPECT_TRUE(direct.gripper.empty());
    const auto &direct_target =
        direct.io.front()->value().target_bindings.front();
    EXPECT_EQ(direct_target.signal_references.size(), mixed ? 2U : 1U);
    EXPECT_EQ(direct_target.capability_bindings.size(), mixed ? 2U : 1U);
    EXPECT_TRUE(direct_target.resource_role_bindings.empty());
    EXPECT_EQ(reload_verified_program_binding(
                  direct.io.front()->canonical_bytes(), bundle, *map, catalog)
                  ->value(),
              direct.io.front()->value());
  }
}

TEST(ProgramBindingCompilerTest,
     OutputRefusesInputSignalAndCapabilityComesFromNodeCatalog) {
  ScopedTrustedRoot root;
  const auto fixture = manyforge::deployment::test::materialize_boolean_io_retained_fixture(
      root.path(), true, true);
  const auto &bundle = *fixture.bundle;
  FilesystemVerifiedRetainedModelMapSource maps(root.path());
  const auto map = maps.get_verified(
      {bundle.retained_bundle_id(), bundle.value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(map);
  FilesystemCatalogArtifactStore catalog(root.path());
  try {
    (void)verify_program_binding_contract(output_binding_program(bundle, false, "ready"), bundle, *map, catalog);
    FAIL() << "wrong signal direction must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &row) { return row.code == "signal_direction_mismatch"; }));
  }
  const auto correct = verify_program_binding_contract(output_binding_program(bundle), bundle, *map, catalog);
  const auto &nodes = correct->execution_table().nodes;
  const auto write = std::find_if(nodes.begin(), nodes.end(),
      [](const auto &node) { return node.instance_id.value() == "write"; });
  ASSERT_NE(write, nodes.end());
  EXPECT_TRUE(std::any_of(write->target_effects.begin(), write->target_effects.end(),
      [](const auto &effect) { return effect.capability_id.value() == "io.gpio.set_output" &&
                                     effect.target.component_id.value() == "device.io" &&
                                     effect.target.command_target_id.value() == "gpio"; }));
}

} // namespace

TEST(ProgramBindingCompilerTest, PreparedOriginIsClosedAndScenarioAccessIsChecked) {
  namespace d = manyforge::deployment;
  ScopedTrustedRoot root;
  const auto product = make_pneumatic_product(root.path());
  const auto prepared = prepare_v3(root.path(), product, "gripper");
  EXPECT_EQ(prepared.origin().scenario_id, "scenario.readback");
  EXPECT_EQ(prepared.origin().project_path,
            "/fixtures/scenario.readback.scenario");
  EXPECT_EQ(
      d::prepared_activation_origin_value(d::parse_prepared_activation_origin(
          d::prepared_activation_origin_value(prepared.origin()))),
      d::prepared_activation_origin_value(prepared.origin()));
  EXPECT_EQ(prepared.normalized_root().find_object_member("scenario_id"), nullptr);
  EXPECT_EQ(prepared.normalized_root().find_object_member("source_document_digest"), nullptr);
  auto origin = prepared.normalized_root().find_object_member("origin")->as_object();
  origin.emplace_back("cell_id", CanonicalValue("foreign"));
  EXPECT_THROW(static_cast<void>(d::parse_prepared_activation_input(reidentify(
      with_member(prepared.normalized_root(), "origin", CanonicalValue(origin))))),
      d::PreparedActivationInputError);
  EXPECT_THROW(static_cast<void>(d::parse_prepared_activation_input(reidentify(
      with_member(prepared.normalized_root(), "schema_version",
                  CanonicalValue("manyforge.prepared_activation_input.v5"))))),
      d::PreparedActivationInputError);
  // Every preparation, the empty cell included, has a Scenario origin.
  EXPECT_THROW(static_cast<void>(d::parse_prepared_activation_origin(with_member(
                   d::prepared_activation_origin_value(prepared.origin()), "kind",
                   CanonicalValue("sealed_canonical_empty")))),
               d::PreparedActivationInputError);
}

TEST(ProgramBindingCompilerTest,
     ScenarioProjectPathIsRequiredNormalizedAndScopesPreparation) {
  namespace d = manyforge::deployment;
  ScopedTrustedRoot root;
  const auto product = make_pneumatic_product(root.path());
  const auto first = prepare_v3(root.path(), product, "gripper",
                             "/fixtures/first/same.scenario");
  const auto second = prepare_v3(root.path(), product, "gripper",
                              "/fixtures/second/same.scenario");
  EXPECT_EQ(first.origin().scenario_id, second.origin().scenario_id);
  EXPECT_EQ(first.origin().source_document_digest,
            second.origin().source_document_digest);
  EXPECT_EQ(first.retained_bundle_id(), second.retained_bundle_id());
  EXPECT_EQ(first.program_revision(), second.program_revision());
  EXPECT_NE(first.prepared_activation_id(), second.prepared_activation_id());
  const auto first_origin = d::prepared_activation_origin_value(first.origin());
  const auto second_origin =
      d::prepared_activation_origin_value(second.origin());
  EXPECT_NE(d::sha256_digest(d::canonical_json(first_origin)),
            d::sha256_digest(d::canonical_json(second_origin)));
  const auto maximum = d::parse_prepared_activation_origin(
      with_member(first_origin, "project_path",
                  CanonicalValue("/" + std::string(4095U, 'x'))));
  EXPECT_EQ(maximum.project_path.size(), 4096U);
  auto missing = first_origin.as_object();
  std::erase_if(missing,
                [](const auto &row) { return row.first == "project_path"; });
  EXPECT_THROW(static_cast<void>(d::parse_prepared_activation_origin(
                   CanonicalValue(missing))),
               d::PreparedActivationInputError);
  for (const auto &invalid : std::vector<std::string>{
           "", "same.scenario", "/fixtures/../same.scenario",
           "/fixtures//same.scenario", "/fixtures/same.scenario/",
           "/" + std::string(4096U, 'x'),
           std::string("/fixtures/a") + '\0' + "b.scenario"}) {
    EXPECT_THROW(
        static_cast<void>(d::parse_prepared_activation_origin(with_member(
            first_origin, "project_path", CanonicalValue(invalid)))),
        d::PreparedActivationInputError)
        << invalid;
  }
}

namespace {

std::string io_binding_program(const RetainedBundle &bundle,
                               std::string_view edge = "level",
                               std::string_view signal_name = "ready") {
  manyforge::deployment::test::ProgramOptions options;
  options.catalog_content_version = bundle.value().catalogs.node_environment.content_version;
  auto root = manyforge::deployment::test::make_program_root(options);
  CanonicalValue::Array children;
  for (const bool wait : {false, true}) {
    CanonicalValue::Object parameters{
        {"signal", CanonicalValue(CanonicalValue::Object{
            {"component_id", CanonicalValue("device.io")},
            {"signal_name", CanonicalValue(std::string(signal_name))}})},
        {"expected_value", CanonicalValue(true)}};
    if (wait) parameters.emplace_back("edge", CanonicalValue(std::string(edge)));
    children.emplace_back(CanonicalValue::Object{
        {"node_id", CanonicalValue(wait ? "io.wait_for_input" : "io.check_input")},
        {"instance_id", CanonicalValue(wait ? "wait" : "check")},
        {"parameters", CanonicalValue(std::move(parameters))},
        {"children", CanonicalValue(CanonicalValue::Array{})}});
  }
  root = with_member(root, "tree", CanonicalValue(CanonicalValue::Object{
      {"node_id", CanonicalValue("sequence")}, {"instance_id", CanonicalValue("root")},
      {"parameters", CanonicalValue(CanonicalValue::Object{})},
      {"children", CanonicalValue(std::move(children))}}));
  return canonical_json(manyforge::deployment::test::with_program_revision(
      manyforge::deployment::normalize_program_authoring(root)));
}

TEST(ProgramBindingCompilerTest, StaticChangedOutsideRefusesIneligibleOutputBeforeBinding) {
  ScopedTrustedRoot root;
  const auto fixture = manyforge::deployment::test::materialize_boolean_io_retained_fixture(
      root.path(), true, true, false, false, false, nullptr, {}, true);
  const auto &bundle = *fixture.bundle;
  FilesystemVerifiedRetainedModelMapSource maps(root.path());
  const auto map = maps.get_verified(
      {bundle.retained_bundle_id(), bundle.value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(map);
  FilesystemCatalogArtifactStore catalog(root.path());
  try {
    (void)verify_program_binding_contract(
        io_binding_program(bundle, "level", "lamp.changed_outside"), bundle, *map, catalog);
    FAIL() << "statically authored changed_outside must reject an ineligible output";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &diagnostic) { return diagnostic.code == "signal_missing"; }));
  }
}

TEST(ProgramBindingCompilerTest, StaticChangedOutsideSealsEligibleBaseAndDerived) {
  ScopedTrustedRoot root;
  const auto fixture = manyforge::deployment::test::materialize_boolean_io_retained_fixture(
      root.path(), true, true);
  const auto &bundle = *fixture.bundle;
  FilesystemVerifiedRetainedModelMapSource maps(root.path());
  const auto map = maps.get_verified(
      {bundle.retained_bundle_id(), bundle.value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(map);
  FilesystemCatalogArtifactStore catalog(root.path());
  const auto program = verify_program_binding_contract(
      io_binding_program(bundle, "level", "lamp.changed_outside"), bundle, *map, catalog);
  const auto binding = compile_program_binding(*program, bundle, *map, catalog);
  ASSERT_NE(binding, nullptr);
  for (const auto *name : {"lamp", "lamp.changed_outside"}) {
    EXPECT_EQ(std::count_if(binding->value().signal_bindings.begin(),
                            binding->value().signal_bindings.end(), [&](const auto &signal) {
      return signal.reference.component_id.value() == "device.io" && signal.reference.signal_name == name;
    }), 1);
  }
  EXPECT_EQ(reload_verified_program_binding(binding->canonical_bytes(), bundle, *map, catalog)->value(),
            binding->value());
}

TEST(ProgramBindingCompilerTest,
     ModelFreeBooleanIoCompilesAndReloadsExactSignalBindings) {
  ScopedTrustedRoot root;
  const auto fixture =
      manyforge::deployment::test::materialize_boolean_io_retained_fixture(
          root.path());
  ASSERT_NE(fixture.bundle, nullptr);
  const auto &bundle = *fixture.bundle;
  EXPECT_TRUE(bundle.value().cell_graph.model_instances.empty());
  EXPECT_TRUE(bundle.value().cell_graph.control_bindings.empty());
  ASSERT_EQ(bundle.value().cell_graph.control_domains.size(), 1U);
  EXPECT_NE(bundle.value().cell_graph.control_domains.front().provider(),
            nullptr);
  EXPECT_TRUE(bundle.value().compiled_products.control_domain_products.empty());
  ASSERT_EQ(bundle.value().compiled_products.symbol_role_maps.size(), 1U);
  FilesystemVerifiedRetainedModelMapSource maps(root.path());
  const auto map = maps.get_verified(
      {bundle.retained_bundle_id(),
       bundle.value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(map);
  const auto *target_map =
      map->find_target(manyforge::core::ControllableComponentId("device.io"),
                       manyforge::core::CommandTargetId("gpio"));
  ASSERT_NE(target_map, nullptr);
  EXPECT_TRUE(target_map->model_groups.empty());
  EXPECT_TRUE(target_map->frames.empty());
  EXPECT_TRUE(target_map->collision_bodies.empty());
  const auto installed =
      manyforge::deployment::ResolveRetainedIoSignalCatalog(bundle.value().cell_graph);
  ASSERT_TRUE(installed.ok());
  ASSERT_EQ(installed.value().size(), 1U);
  EXPECT_EQ(installed.value().front().provider.initial_value, false);
  EXPECT_EQ(installed.value().front().provider.ingress_depth, 64U);
  FilesystemCatalogArtifactStore catalog(root.path());
  const auto program =
      verify_program_binding_contract(io_binding_program(bundle), bundle, *map, catalog);
  const auto binding =
      compile_program_binding(*program, bundle, *map, catalog);
  ASSERT_NE(binding, nullptr);
  const auto direct =
      compile_direct_control_program_binding_inventory(bundle, *map, catalog);
  EXPECT_TRUE(direct.motion.empty());
  EXPECT_TRUE(direct.gripper.empty());
  ASSERT_EQ(direct.io.size(), 1U);
  EXPECT_EQ(direct.io.front()->value().program_id.value(),
            manyforge::deployment::kDirectControlIoProgramId);
  ASSERT_EQ(direct.io.front()->value().target_bindings.size(), 1U);
  EXPECT_TRUE(direct.io.front()
                  ->value()
                  .target_bindings.front()
                  .resource_role_bindings.empty());
  EXPECT_EQ(direct.io.front()->value().target_bindings.front().signal_references.size(), 1U);
  EXPECT_EQ(reload_verified_program_binding(
                direct.io.front()->canonical_bytes(), bundle, *map, catalog)
                ->value(),
            direct.io.front()->value());
  ASSERT_EQ(binding->value().target_bindings.size(), 1U);
  const auto &target = binding->value().target_bindings.front();
  EXPECT_TRUE(target.resource_role_bindings.empty());
  ASSERT_EQ(target.signal_references.size(), 1U);
  EXPECT_EQ(target.signal_references.front().component_id.value(), "device.io");
  EXPECT_EQ(target.signal_references.front().signal_name, "ready");
  ASSERT_EQ(binding->value().signal_bindings.size(), 1U);
  EXPECT_EQ(binding->value().signal_bindings.front().reference, target.signal_references.front());
  ASSERT_EQ(target.capability_bindings.size(), 2U);
  EXPECT_TRUE(target.capability_bindings.front().capability_provider_binding);
  EXPECT_FALSE(target.capability_bindings.front().control_binding);
  const auto reopened = manyforge::deployment::reload_verified_program_binding(
      binding->canonical_bytes(), bundle, *map, catalog);
  EXPECT_EQ(reopened->value(), binding->value());
  auto forged = binding->value();
  forged.signal_bindings.front().signal_descriptor_digest =
      manyforge::core::ContentDigest(std::string(kZeroDigest));
  const auto resealed =
      manyforge::deployment::detail::ProgramBindingConstructionAuthority::seal(
          forged);
  EXPECT_THROW(
      static_cast<void>(manyforge::deployment::reload_verified_program_binding(
          resealed->canonical_bytes(), bundle, *map, catalog)),
      manyforge::deployment::ProgramBindingError);
  try {
    (void)verify_program_binding_contract(io_binding_program(bundle, "rising"), bundle, *map, catalog);
    FAIL() << "unsupported input edge must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
        [](const auto &row) { return row.code == "io_watch_condition_unsupported"; }));
  }
}

TEST(ProgramBindingCompilerTest, CompilesPortableReleaseWithoutChangingItsEnvelope) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), true, false, false, true);
  auto program = make_gripper_program_root(*context.bundle);
  auto policies = program.find_object_member("contact_policies")->as_array();
  auto policy = policies.front();
  auto zones = policy.find_object_member("zones")->as_array();
  auto zone = zones.front().as_object();
  std::erase_if(zone, [](const auto& field) {
    return field.first == "max_penetration_m" ||
           field.first == "terminal_window_start_fraction";
  });
  zone.emplace_back("release_evidence_mode", CanonicalValue("signed_penetration"));
  zone.emplace_back("max_start_penetration_m", CanonicalValue(0.002));
  zones.front() = CanonicalValue(std::move(zone));
  policy = with_member(policy, "mode", CanonicalValue("contact_release"));
  policy = with_member(policy, "zones", CanonicalValue(std::move(zones)));
  policy = with_member(policy, "version", CanonicalValue(compute_content_identity(
      policy, ContentIdentitySpec{.identity_domain = "catalog-record-revision/contact_policy",
                                  .api_version = "manyforge.contact_policy.v1",
                                  .own_field = "version"})));
  policies.front() = policy;
  program = with_member(program, "contact_policies", CanonicalValue(std::move(policies)));
  program = with_member(program, "program_revision", CanonicalValue(compute_content_identity(
      program, ContentIdentitySpec{.identity_domain = "program-revision",
                                   .api_version = "manyforge.program.v3",
                                   .own_field = "program_revision"})));
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto contract = verify_program_binding_contract(canonical_json(program), *context.bundle, context.model_map, catalog_store);
  ASSERT_EQ(contract->value().contact_grasp_templates.size(), 1U);
  const auto& release = std::get<manyforge::core::ContactReleaseEnvelopeDTO>(
      contract->value().contact_grasp_templates.front().envelope);
  EXPECT_DOUBLE_EQ(release.max_start_penetration_m, 0.002);
  EXPECT_TRUE(release.require_separation_by_end);
  EXPECT_TRUE(release.require_strictly_decreasing_penetration);
  EXPECT_FALSE(release.allow_recontact);
  const auto binding = compile_program_binding(
      *contract, *context.bundle, context.model_map, catalog_store);
  ASSERT_EQ(binding->value().contact_grasp_bindings.size(), 1U);
  const auto& bound = binding->value().contact_grasp_bindings.front();
  EXPECT_EQ(bound.envelope, contract->value().contact_grasp_templates.front().envelope);
  EXPECT_EQ(bound.runtime_object_id.value(), "selected_object");
  ASSERT_EQ(bound.allowed_body_bindings.size(), 1U);
  EXPECT_EQ(bound.allowed_body_bindings.front().body.component_id.value(), kLiteGripperComponentId);
  const auto& body = bound.allowed_body_bindings.front();
  EXPECT_EQ(body.body.collision_body_id.value(), "gripper_collision");
  ASSERT_FALSE(body.collision_body_ids.empty());
  for (std::size_t index = 0; index < body.collision_body_ids.size(); ++index)
    EXPECT_EQ(body.collision_body_ids[index], manyforge::core::make_component_collision_body_id(
        body.body.component_id, body.model_instance_id,
        manyforge::core::CollisionBodyRoleId(body.body.collision_body_id.value()), index));
  EXPECT_EQ(manyforge::deployment::reload_verified_program_binding(
                binding->canonical_bytes(), *context.bundle, context.model_map, catalog_store)->value(),
            binding->value());
}

TEST(ProgramBindingCompilerTest,
     InputReadOfCompiledSystemSignalBindsWithoutTargetInputCapability) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), true);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const std::vector<std::pair<std::string_view, std::string_view>> cases{
      {"io.gpio.get_input", "io.check_input"},
      {"io.gpio.wait_for_input", "io.wait_for_input"}};
  const std::string signal_name = "carried_object_lost." + std::string(kLite6GripperTargetId);
  for (const auto &[capability, node_id] : cases) {
    manyforge::deployment::test::ProgramOptions options;
    options.catalog_content_version = context.bundle->value().catalogs.node_environment.content_version;
    auto program = manyforge::deployment::test::make_program_root(options);
    program = with_member(program, "tree", CanonicalValue(CanonicalValue::Object{
        {"node_id", CanonicalValue(std::string(node_id))}, {"instance_id", CanonicalValue("read")},
        {"parameters", CanonicalValue(CanonicalValue::Object{
            {"signal", CanonicalValue(CanonicalValue::Object{
                {"component_id", CanonicalValue(std::string(kLiteGripperComponentId))},
                {"signal_name", CanonicalValue(signal_name)}})},
            {"expected_value", CanonicalValue(true)}})},
        {"children", CanonicalValue(CanonicalValue::Array{})}}));
    program = manyforge::deployment::test::with_program_revision(
        manyforge::deployment::normalize_program_authoring(program));
    const auto contract = verify_program_binding_contract(canonical_json(program), *context.bundle, context.model_map, catalog_store);
    const auto binding = compile_program_binding(*contract, *context.bundle, context.model_map, catalog_store);
    ASSERT_NE(binding, nullptr);
    const auto &targets = binding->value().target_bindings;
    const auto gripper = std::find_if(targets.begin(), targets.end(), [](const auto &row) {
      return row.target.component_id.value() == kLiteGripperComponentId &&
             row.target.command_target_id.value() == kLite6GripperTargetId;
    });
    ASSERT_NE(gripper, targets.end());
    ASSERT_EQ(gripper->signal_references.size(), 1U);
    EXPECT_EQ(gripper->signal_references.front().component_id.value(), kLiteGripperComponentId);
    EXPECT_EQ(gripper->signal_references.front().signal_name, signal_name);
    ASSERT_EQ(binding->value().signal_bindings.size(), 1U);
    EXPECT_EQ(binding->value().signal_bindings.front().direction, manyforge::core::IoSignalDirection::kInput);
    // The system signal serves the read; no input capability is invented.
    EXPECT_TRUE(std::none_of(gripper->capability_bindings.begin(), gripper->capability_bindings.end(),
        [&](const auto &row) { return row.capability_id.value() == capability; }));
    EXPECT_EQ(manyforge::deployment::reload_verified_program_binding(
                  binding->canonical_bytes(), *context.bundle, context.model_map, catalog_store)->value(),
              binding->value());
  }
}

TEST(ProgramBindingCompilerTest, MissingInputIsRefusedEvenBesideGripperSystemSignal) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), true);
  FilesystemCatalogArtifactStore catalog(root.path());
  const std::string system_signal = "carried_object_lost." + std::string(kLite6GripperTargetId);
  for (const bool include_system : {false, true}) {
    manyforge::deployment::test::ProgramOptions options;
    options.catalog_content_version = context.bundle->value().catalogs.node_environment.content_version;
    auto program = manyforge::deployment::test::make_program_root(options);
    CanonicalValue::Array reads;
    const auto add_read = [&](std::string signal) {
      reads.emplace_back(CanonicalValue::Object{
          {"node_id", CanonicalValue("io.check_input")},
          {"instance_id", CanonicalValue("read_" + std::to_string(reads.size()))},
          {"parameters", CanonicalValue(CanonicalValue::Object{
              {"signal", CanonicalValue(CanonicalValue::Object{
                  {"component_id", CanonicalValue(std::string(kLiteGripperComponentId))},
                  {"signal_name", CanonicalValue(std::move(signal))}})},
              {"expected_value", CanonicalValue(true)}})},
          {"children", CanonicalValue(CanonicalValue::Array{})}});
    };
    if (include_system) add_read(system_signal);
    add_read("fingers");
    program = with_member(program, "tree", CanonicalValue(CanonicalValue::Object{
        {"node_id", CanonicalValue("sequence")}, {"instance_id", CanonicalValue("root")},
        {"parameters", CanonicalValue(CanonicalValue::Object{})},
        {"children", CanonicalValue(std::move(reads))}}));
    program = manyforge::deployment::test::with_program_revision(
        manyforge::deployment::normalize_program_authoring(program));
    const auto diagnostics = manyforge::deployment::validate_program_references(
        program, false, *context.bundle, context.model_map, catalog);
    ASSERT_EQ(diagnostics.size(), 1U);
    EXPECT_EQ(diagnostics.front().code, "signal_missing");
    EXPECT_EQ(diagnostics.front().location.node_instance_id,
              include_system ? "read_1" : "read_0");
  }
}

// A v2 Program could declare an unused target input capability independently
// of its nodes. v3 has no such declaration: each actual input use is checked
// independently above, and the system-signal success case remains separate.

} // namespace
