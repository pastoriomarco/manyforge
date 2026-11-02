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
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/initial_scene_projection.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "content_identity_internal.hpp"
#include "program_binding_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "gripper_only_retention_fixture.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#include "io_retained_fixture.hpp"
#include "program_binding_internal.hpp"

namespace {

using manyforge::core::CapabilityId;
using manyforge::core::ComponentRoleId;
using manyforge::core::ProgramComponentRoleAssignmentDTO;
using manyforge::core::ProgramRoleAssignmentDTO;
using manyforge::core::ProgramTargetRoleAssignmentDTO;
using manyforge::core::TargetRoleId;
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
             bool shared_arm_endpoint_bodies = false) {
  const auto materialized = [&] {
    if (!gripper_only) {
      if (endpoint_contact_bodies) {
        return materialize_lite6_ros2_control_observed_endpoint_retained_consumer_fixture(
            root, shared_arm_endpoint_bodies);
      }
      return materialize_lite6_ros2_control_observed_retained_consumer_fixture(root);
    }
    // The Lite gripper as its own component (a separate tool).
    return manyforge::deployment::test::materialize_lite6_retained_consumer_fixture_from(
        root, manyforge::deployment::test::make_lite_gripper_only_retention_fixture(
                  manyforge::deployment::test::kWithLifecycleOffers));
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
          root, "i6_lite6_motion_cell");
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
      {"program_id", CanonicalValue("i6_binding_compiler_program")},
      {"program_revision", CanonicalValue(std::string(kZeroDigest))},
      {"program_profile", CanonicalValue("composable-cell-v1")},
      {"name", CanonicalValue("I6 binding compiler fixture")},
      {"catalog",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.node_catalog.v2")},
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

/// A Program over the Lite gripper component alone (role `tool`): its
/// gripper target plus a contact grasp policy on the gripper's body.
[[nodiscard]] CanonicalValue
make_gripper_program_root(const RetainedBundle &bundle) {
  CanonicalValue::Object root = make_program_root(bundle).as_object();
  for (auto &[name, value] : root) {
    if (name == "component_roles") {
      value = CanonicalValue(CanonicalValue::Object{
          {"tool",
           CanonicalValue(CanonicalValue::Object{
               {"required_component_type_tags",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("articulated-gripper")})},
               {"required_model_roles", CanonicalValue(CanonicalValue::Array{
                                            CanonicalValue("gripper_model")})},
           })},
      });
    } else if (name == "target_roles") {
      CanonicalValue::Object targets;
      targets.emplace_back(
          "gripper",
          CanonicalValue(CanonicalValue::Object{
              {"component_role", CanonicalValue("tool")},
              {"required_capabilities",
               CanonicalValue(
                   CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                       {"capability_id",
                        CanonicalValue("tool.gripper.command")},
                       {"parameters", CanonicalValue(CanonicalValue::Object{})},
                       {"disposition", CanonicalValue("required")},
                   })})},
              {"required_frame_roles",
               CanonicalValue(CanonicalValue::Array{CanonicalValue("tcp")})},
              {"required_resource_roles", CanonicalValue(CanonicalValue::Array{
                                              CanonicalValue("fingers")})},
              {"required_collision_body_roles",
               CanonicalValue(
                   CanonicalValue::Array{CanonicalValue("gripper_collision")})},
              {"required_model_roles", CanonicalValue(CanonicalValue::Array{
                                           CanonicalValue("gripper_model")})},
          }));
      value = CanonicalValue(std::move(targets));
    }
  }
  CanonicalValue contact_policy(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.contact_policy.v1")},
      {"policy_id", CanonicalValue("selected_object_grasp")},
      {"version", CanonicalValue(std::string(kZeroDigest))},
      {"mode", CanonicalValue("contact_grasp")},
      {"sensing_mode", CanonicalValue("geometry_only")},
      {"default_unmatched_response", CanonicalValue("hard_stop")},
      {"zones",
       CanonicalValue(
           CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
               {"zone_id", CanonicalValue("tool_contact")},
               {"body_pairs",
                CanonicalValue(
                    CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                        {"body_a",
                         CanonicalValue(CanonicalValue::Object{
                             {"kind", CanonicalValue("component_model_body")},
                             {"component_role_id", CanonicalValue("tool")},
                             {"model_role_id", CanonicalValue("gripper_model")},
                             {"collision_body_role_id",
                              CanonicalValue("gripper_collision")},
                         })},
                        {"body_b",
                         CanonicalValue(CanonicalValue::Object{
                             {"kind", CanonicalValue("runtime_object")},
                             {"runtime_object_id",
                              CanonicalValue("selected_object")},
                         })},
                    })})},
               {"violation_response", CanonicalValue("hard_stop")},
               {"max_penetration_m", CanonicalValue(0.002)},
               {"terminal_window_start_fraction", CanonicalValue(0.8)},
           })})},
  });
  const std::string policy_revision = compute_content_identity(
      contact_policy,
      ContentIdentitySpec{
          .identity_domain = "catalog-record-revision/contact_policy",
          .api_version = "manyforge.contact_policy.v1",
          .own_field = "version",
      });
  auto policy_members = contact_policy.as_object();
  for (auto &[name, value] : policy_members) {
    if (name == "version") {
      value = CanonicalValue(policy_revision);
    }
  }
  root.emplace_back(
      "blackboard",
      CanonicalValue(CanonicalValue::Object{
          {"namespaces", CanonicalValue(CanonicalValue::Array{})},
          {"keys", CanonicalValue(CanonicalValue::Array{
                       CanonicalValue(CanonicalValue::Object{
                           {"key", CanonicalValue("selected_object")},
                           {"type", CanonicalValue("scene_resource")},
                           {"access", CanonicalValue("read_write")},
                           {"persistence", CanonicalValue("run")},
                       })})},
      }));
  root.emplace_back("contact_policies",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(std::move(policy_members))}));
  return CanonicalValue(std::move(root));
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

[[nodiscard]] ProgramRoleAssignmentDTO valid_assignment() {
  return {
      {ProgramComponentRoleAssignmentDTO{
          ComponentRoleId("robot"),
          manyforge::core::ControllableComponentId(
              std::string(kLite6ControllableComponentId))}},
      {ProgramTargetRoleAssignmentDTO{
          TargetRoleId("arm"),
          manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId))}},
  };
}

[[nodiscard]] ProgramRoleAssignmentDTO gripper_assignment() {
  return {
      {ProgramComponentRoleAssignmentDTO{
          ComponentRoleId("tool"),
          manyforge::core::ControllableComponentId(
              std::string(kLiteGripperComponentId))}},
      {ProgramTargetRoleAssignmentDTO{
          TargetRoleId("gripper"),
          manyforge::core::CommandTargetId(std::string(kLite6GripperTargetId))}},
  };
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
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());

  const auto binding =
      compile_program_binding(*contract, valid_assignment(), *context.bundle,
                              context.model_map, catalog_store);

  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->value().program_id, contract->value().program_id);
  EXPECT_EQ(binding->value().program_revision,
            contract->value().program_revision);
  EXPECT_EQ(binding->value().retained_bundle_id,
            context.bundle->retained_bundle_id());
  ASSERT_EQ(binding->value().component_role_bindings.size(), 1U);
  EXPECT_EQ(binding->value()
                .component_role_bindings.front()
                .controllable_component_id.value(),
            kLite6ControllableComponentId);
  ASSERT_EQ(binding->value().target_role_bindings.size(), 1U);
  const auto &target = binding->value().target_role_bindings.front();
  EXPECT_EQ(target.command_target_id.value(), kLite6ArmTargetId);
  ASSERT_EQ(target.frame_role_bindings.size(), 1U);
  ASSERT_EQ(target.resource_role_bindings.size(), 1U);
  EXPECT_EQ(target.resource_role_bindings.front().resource_ids.size(), 6U);
  ASSERT_EQ(target.capability_bindings.size(), 1U);
  EXPECT_EQ(target.capability_bindings.front().capability_id.value(),
            "state.observed.current");
  EXPECT_TRUE(target.capability_bindings.front().control_binding.has_value());
  EXPECT_FALSE(target.capability_bindings.front().command_schema.has_value());
  EXPECT_EQ(binding->value().provenance.source_records,
            context.bundle->value().provenance.source_records);
  EXPECT_FALSE(binding->value().provenance.resolved_output_records.empty());
}

TEST(ProgramBindingCompilerTest,
     PlanningOnlyProgramStillCarriesTcpInventoryForExecutionCapableTarget) {
  ScopedTrustedRoot root;
  // The observed ROS fixture retains both required definitions (pose goal and
  // state.observed.current) on a target that also offers trajectory execution.
  const BindingCompilerContext context = make_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle, false, false, true),
      *context.bundle);
  const auto binding = compile_program_binding(
      *contract, valid_assignment(), *context.bundle, context.model_map,
      catalog_store);

  ASSERT_NE(binding, nullptr);
  ASSERT_EQ(binding->value().target_role_bindings.size(), 1U);
  const auto &target = binding->value().target_role_bindings.front();
  ASSERT_EQ(target.capability_bindings.size(), 2U);
  EXPECT_FALSE(std::any_of(
      target.capability_bindings.begin(), target.capability_bindings.end(),
      [](const auto &capability) {
        return capability.capability_id.value() ==
               "execution.trajectory.execute";
      }));
  ASSERT_TRUE(target.robot_tcps.has_value());
  ASSERT_EQ(target.robot_tcps->size(), 2U);
  EXPECT_EQ(target.robot_tcps->front().entry.frame_role_id.value(), "flange");
  EXPECT_EQ(target.robot_tcps->back().entry.frame_role_id.value(), "tcp");
}

TEST(ProgramBindingCompilerTest,
     CopiesVerifiedNodePoliciesAndLeavesDirectControlEmpty) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle, true, true), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto binding =
      compile_program_binding(*contract, valid_assignment(), *context.bundle,
                              context.model_map, catalog_store);

  ASSERT_EQ(contract->value().node_policies.size(), 1U);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->value().node_policies, contract->value().node_policies);
  EXPECT_EQ(binding->value()
                .node_policies.front()
                .recovery_policy.intervention_stop_scope,
            manyforge::core::ProgramInterventionStopScope::kOwnGroups);

  ScopedTrustedRoot direct_root;
  const BindingCompilerContext direct_context =
      make_ros_motion_context(direct_root.path());
  FilesystemCatalogArtifactStore direct_catalog_store(direct_root.path());
  const auto direct = compile_direct_control_motion_program_binding(
      manyforge::core::ControllableComponentId(
          std::string(kLite6ControllableComponentId)),
      manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
      *direct_context.bundle, direct_context.model_map, direct_catalog_store);
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
  ASSERT_EQ(first->value().component_role_bindings.size(), 1U);
  EXPECT_EQ(
      first->value().component_role_bindings.front().component_role_id.value(),
      manyforge::deployment::kDirectControlComponentRoleId);
  ASSERT_EQ(first->value().target_role_bindings.size(), 1U);
  const auto &target = first->value().target_role_bindings.front();
  EXPECT_EQ(target.target_role_id.value(),
            manyforge::deployment::kDirectControlTargetRoleId);
  EXPECT_EQ(target.command_target_id.value(), kLite6ArmTargetId);
  EXPECT_FALSE(target.frame_role_bindings.empty());
  EXPECT_NE(std::find_if(
                target.frame_role_bindings.begin(),
                target.frame_role_bindings.end(),
                [](const auto &frame) {
                  return frame.frame_role_id.value() ==
                         manyforge::deployment::kDirectControlBaseFrameRoleId;
                }),
            target.frame_role_bindings.end());
  // C-P3: the binding holds `base` and exactly the target's tagged TCPs
  // (`flange` and `tcp` on this arm, as on the maintained Lite6 arm).
  std::vector<std::string> frame_roles;
  for (const auto &frame : target.frame_role_bindings)
    frame_roles.push_back(frame.frame_role_id.value());
  EXPECT_EQ(frame_roles, (std::vector<std::string>{"base", "flange", "tcp"}));
  EXPECT_FALSE(target.resource_role_bindings.empty());
  EXPECT_FALSE(target.collision_body_role_bindings.empty());
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

// B2: a target frame role binds the target's own row, and an absent role
// binds nothing; the direct-control binding holds `base` and the target's
// tagged TCPs (C-P3).
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
                direct->canonical_bytes(), *context.bundle, context.model_map)
                ->value(),
            direct->value());
  auto forged = direct->value();
  ASSERT_TRUE(forged.target_role_bindings.front().robot_tcps);
  ASSERT_FALSE(forged.target_role_bindings.front().robot_tcps->empty());
  forged.target_role_bindings.front().robot_tcps->front().frame.compiled_frame_name =
      "foreign_tcp_frame";
  const auto resealed = manyforge::deployment::detail::
      ProgramBindingConstructionAuthority::seal(std::move(forged));
  EXPECT_THROW(static_cast<void>(manyforge::deployment::reload_verified_program_binding(
                   resealed->canonical_bytes(), *context.bundle, context.model_map)),
               manyforge::deployment::ProgramBindingError);
  const auto &frames = direct->value().target_role_bindings.front().frame_role_bindings;
  std::vector<std::string> roles;
  for (const auto &frame : frames) roles.push_back(frame.frame_role_id.value());
  EXPECT_EQ(roles, (std::vector<std::string>{"base", "flange", "tcp"}));
}

// C-RQ8: a declared frame role binds as itself only; a default TCP named
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

// TCP-C/B1: the direct-control TCP list of a robot without a tool lists its
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
    ASSERT_TRUE(tcp.target_role_id.has_value());
    EXPECT_EQ(tcp.target_role_id->value(),
              manyforge::deployment::kDirectControlTargetRoleId);
    EXPECT_EQ(tcp.binding.entry.component_id, component);
    EXPECT_EQ(tcp.binding.entry.target_id.value(), kLite6ArmTargetId);
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
  ASSERT_EQ(first->value().target_role_bindings.size(), 1U);
  const auto &target = first->value().target_role_bindings.front();
  EXPECT_EQ(target.target_role_id.value(),
            manyforge::deployment::kDirectControlTargetRoleId);
  EXPECT_EQ(target.command_target_id.value(), kLite6GripperTargetId);
  EXPECT_FALSE(target.robot_tcps.has_value());
  EXPECT_EQ(first->canonical_bytes().find("\"robot_tcps\""), std::string::npos);
  const auto reloaded = manyforge::deployment::reload_verified_program_binding(
      first->canonical_bytes(), *context.bundle, context.model_map);
  ASSERT_NE(reloaded, nullptr);
  EXPECT_FALSE(reloaded->value().target_role_bindings.front().robot_tcps);
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
  EXPECT_EQ(first_motion.motion.front()->canonical_bytes(),
            second_motion.motion.front()->canonical_bytes());

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
  EXPECT_EQ(first_gripper.gripper.front()->canonical_bytes(),
            second_gripper.gripper.front()->canonical_bytes());
}

TEST(ProgramBindingCompilerTest, RejectsIncompleteAndUnknownRoleAssignments) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());

  ProgramRoleAssignmentDTO missing = valid_assignment();
  missing.target_roles.clear();
  EXPECT_THROW(static_cast<void>(compile_program_binding(
                   *contract, std::move(missing), *context.bundle,
                   context.model_map, catalog_store)),
               ProgramBindingCompilerError);

  ProgramRoleAssignmentDTO unknown = valid_assignment();
  unknown.target_roles.front().command_target_id =
      manyforge::core::CommandTargetId("unknown_target");
  EXPECT_THROW(static_cast<void>(compile_program_binding(
                   *contract, std::move(unknown), *context.bundle,
                   context.model_map, catalog_store)),
               ProgramBindingCompilerError);
}

TEST(ProgramBindingCompilerTest,
     OutcomeBindsExactlyLikeTheCompilerWhenEveryRoleResolves) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());

  const auto outcome = compile_program_binding_outcome(
      *contract, valid_assignment(), *context.bundle, context.model_map,
      catalog_store);
  const auto binding =
      compile_program_binding(*contract, valid_assignment(), *context.bundle,
                              context.model_map, catalog_store);

  ASSERT_NE(outcome.binding, nullptr);
  EXPECT_FALSE(outcome.non_executable.has_value());
  EXPECT_EQ(outcome.binding->canonical_bytes(), binding->canonical_bytes());
}

TEST(ProgramBindingCompilerTest,
     MissingCandidateTargetMakesTheProgramNamedNonExecutable) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  auto missing_target = valid_assignment();
  missing_target.target_roles.front().command_target_id =
      manyforge::core::CommandTargetId("removed_target");

  const auto outcome = compile_program_binding_outcome(
      *contract, missing_target, *context.bundle, context.model_map,
      catalog_store);

  EXPECT_EQ(outcome.binding, nullptr);
  ASSERT_TRUE(outcome.non_executable.has_value());
  const auto &value = *outcome.non_executable;
  EXPECT_TRUE(value.valid());
  EXPECT_EQ(value.program_id, contract->value().program_id);
  EXPECT_EQ(value.program_revision, contract->value().program_revision);
  EXPECT_EQ(value.retained_bundle_id, context.bundle->retained_bundle_id());
  ASSERT_EQ(value.unresolved_roles.size(), 1U);
  const auto &row = value.unresolved_roles.front();
  EXPECT_EQ(row.component_role_id, ComponentRoleId("robot"));
  EXPECT_EQ(row.target_role_id, TargetRoleId("arm"));
  EXPECT_EQ(row.command_target_id,
            manyforge::core::CommandTargetId("removed_target"));
  EXPECT_EQ(row.reason, ProgramBindingUnresolvedReason::kTargetMissing);
  // No authored node uses the role, but the Program still cannot bind.
  EXPECT_TRUE(value.affected_nodes.empty());
  EXPECT_EQ(DecodeProgramNonExecutableOutcome(
                EncodeProgramNonExecutableOutcome(value)),
            value);
}

TEST(ProgramBindingCompilerTest,
     MissingComponentNamesItsRoleAndEveryDependentTarget) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  auto missing_component = valid_assignment();
  missing_component.component_roles.front().controllable_component_id =
      manyforge::core::ControllableComponentId("removed_robot");

  const auto outcome = compile_program_binding_outcome(
      *contract, missing_component, *context.bundle, context.model_map,
      catalog_store);

  ASSERT_TRUE(outcome.non_executable.has_value());
  const auto &rows = outcome.non_executable->unresolved_roles;
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_FALSE(rows[0].target_role_id.has_value());
  EXPECT_EQ(rows[0].reason, ProgramBindingUnresolvedReason::kComponentMissing);
  EXPECT_EQ(rows[0].controllable_component_id,
            manyforge::core::ControllableComponentId("removed_robot"));
  EXPECT_EQ(rows[1].target_role_id, TargetRoleId("arm"));
  EXPECT_EQ(rows[1].reason,
            ProgramBindingUnresolvedReason::kComponentRoleUnresolved);
  // The ordinary compiler keeps failing exactly as before.
  EXPECT_THROW(static_cast<void>(compile_program_binding(
                   *contract, missing_component, *context.bundle,
                   context.model_map, catalog_store)),
               ProgramBindingCompilerError);
}

TEST(ProgramBindingCompilerTest,
     MalformedAssignmentStillAbortsInsteadOfBecomingNonExecutable) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  auto incomplete = valid_assignment();
  incomplete.target_roles.clear();

  EXPECT_THROW(static_cast<void>(compile_program_binding_outcome(
                   *contract, std::move(incomplete), *context.bundle,
                   context.model_map, catalog_store)),
               ProgramBindingCompilerError);
}

// -- Prepared-activation outcome readback (H1a) ------------------------------

[[nodiscard]] CanonicalValue assignment_value(std::string_view command_target) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_roles",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
           {"component_role_id", CanonicalValue("robot")},
           {"controllable_component_id",
            CanonicalValue(std::string(kLite6ControllableComponentId))},
       })})},
      {"target_roles",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
           {"command_target_id", CanonicalValue(std::string(command_target))},
           {"target_role_id", CanonicalValue("arm")},
       })})},
  });
}

[[nodiscard]] manyforge::deployment::PreparedActivationInput
prepare(const std::filesystem::path &root,
        const BindingCompilerContext &context, std::string_view command_target,
        std::string project_path = "/fixtures/scenario.readback.scenario") {
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root);
  manyforge::deployment::FilesystemRawArtifactStore raw_store(root);
  return manyforge::deployment::assemble_prepared_activation(
      {manyforge::deployment::PreparedScenarioOrigin{
           "scenario.readback",
           manyforge::core::ContentDigest("sha256:" + std::string(64U, '1')),
           std::move(project_path)},
       manyforge::core::ContentDigest("sha256:" + std::string(64U, '2')),
       "optional"},
      *context.bundle, context.model_map, *contract,
      assignment_value(command_target), catalog_store, raw_store);
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

TEST(ProgramBindingCompilerTest,
     ReadbackRecompilesANonExecutableOutcomeAndRejectsATamperedResult) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto honest = prepare(root.path(), context, "removed_target");
  ASSERT_TRUE(honest.program_outcome().has_value());
  EXPECT_FALSE(honest.program_binding_id().has_value());
  EXPECT_NO_THROW(manyforge::deployment::verify_prepared_program_outcome(honest));

  // Another valid named reason, same Program/bundle/graph: identity and
  // parsing pass, but it is not what the preserved assignment compiles to.
  const auto &outcome_value = honest.normalized_root()
                                  .find_object_member("program_outcome")
                                  ->find_object_member("outcome");
  auto rows = outcome_value->find_object_member("unresolved_roles")->as_array();
  rows.front() = with_member(rows.front(), "reason",
                             CanonicalValue("target_incompatible"));
  const auto tampered_outcome = with_member(
      *outcome_value, "unresolved_roles", CanonicalValue(std::move(rows)));
  const auto tampered = manyforge::deployment::parse_prepared_activation_input(
      reidentify(with_member(
          honest.normalized_root(), "program_outcome",
          with_member(*honest.normalized_root().find_object_member("program_outcome"),
                      "outcome", tampered_outcome))));
  EXPECT_THROW(manyforge::deployment::verify_prepared_program_outcome(tampered),
               manyforge::deployment::PreparedActivationInputError);

  // The Manager's admission path refuses it; the honest value reads back.
  ScopedTrustedRoot store;
  const manyforge::deployment::FilesystemPreparedActivationRepository repository(
      store.path());
  ASSERT_TRUE(repository.put_if_absent(honest));
  ASSERT_TRUE(repository.put_if_absent(tampered));
  EXPECT_NE(repository.get_verified(honest.prepared_activation_id()), nullptr);
  EXPECT_THROW(static_cast<void>(
                   repository.get_verified(tampered.prepared_activation_id())),
               manyforge::deployment::PreparedActivationInputError);
}

TEST(ProgramBindingCompilerTest,
     PreparedActivationStoreKeepsOnlyTheMostRecentlyWrittenValues) {
  namespace d = manyforge::deployment;
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
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
    values.push_back(prepare(root.path(), context, kLite6ArmTargetId,
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
  ASSERT_TRUE(repository.put_if_absent(prepare(
      root.path(), context, kLite6ArmTargetId, "/fixtures/scenario-new.readback.scenario")));
  EXPECT_EQ(stored_count(), d::kMaximumRetainedPreparedActivations);
  EXPECT_NE(repository.get_verified(values[kExtra].prepared_activation_id()),
            nullptr);
  EXPECT_EQ(repository.get_verified(values[kExtra + 1U].prepared_activation_id()),
            nullptr);
}

TEST(ProgramBindingCompilerTest, NoProgramReadbackProvesAbsenceWithoutInventingABinding) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto bound = prepare(root.path(), context, kLite6ArmTargetId);
  auto payload = *bound.normalized_root().find_object_member("activation_payload");
  payload = with_member(payload, "program", CanonicalValue(nullptr));
  payload = with_member(payload, "program_binding", CanonicalValue(nullptr));
  payload = with_member(payload, "program_role_assignment", CanonicalValue(CanonicalValue::Object{
      {"component_roles", CanonicalValue(CanonicalValue::Array{})},
      {"target_roles", CanonicalValue(CanonicalValue::Array{})},
  }));
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
  for (const auto field : {"program", "program_binding", "program_role_assignment"}) {
    const auto forged_payload = with_member(payload, field,
        *bound.normalized_root().find_object_member("activation_payload")->find_object_member(field));
    EXPECT_THROW(static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
        reidentify(with_member(value, "activation_payload", forged_payload)))),
        manyforge::deployment::PreparedActivationInputError);
  }
  for (const auto field : {"program_id", "program_revision"}) {
    EXPECT_THROW(static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
        reidentify(with_member(value, field, *bound.normalized_root().find_object_member(field))))),
        manyforge::deployment::PreparedActivationInputError);
  }
  EXPECT_THROW(static_cast<void>(manyforge::deployment::parse_prepared_activation_input(
      reidentify(with_member(value, "schema_version", CanonicalValue("manyforge.prepared_activation_input.v3"))))),
      manyforge::deployment::PreparedActivationInputError);
}

TEST(ProgramBindingCompilerTest,
     ReadbackRefusesTheRemovedParameterOverrideForm) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto bound = prepare(root.path(), context, kLite6ArmTargetId);
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
  const BindingCompilerContext context = make_context(root.path());
  const auto bound = prepare(root.path(), context, kLite6ArmTargetId);
  ASSERT_TRUE(bound.program_binding_id().has_value());
  const auto honest_unbound = prepare(root.path(), context, "removed_target");

  // Claim "non-executable" (and drop the binding) for a Program whose
  // carried assignment actually binds.
  auto payload = with_member(
      *bound.normalized_root().find_object_member("activation_payload"),
      "program_binding", CanonicalValue(nullptr));
  const auto forged_root = with_member(
      with_member(bound.normalized_root(), "activation_payload", std::move(payload)),
      "program_outcome",
      *honest_unbound.normalized_root().find_object_member("program_outcome"));
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

/// The arm Program with an authored initial Scene: one object on the
/// robot's arm-model `tcp` frame and one in the cell frame.
[[nodiscard]] std::string program_with_scene(const RetainedBundle &bundle) {
  CanonicalValue::Object members = make_program_root(bundle).as_object();
  CanonicalValue scene(CanonicalValue::Array{
      scene_box("on_arm_tcp",
                CanonicalValue(CanonicalValue::Object{
                    {"component_role", CanonicalValue("robot")},
                    {"frame_role", CanonicalValue("tcp")},
                    {"kind", CanonicalValue("component_frame")},
                    {"model_role", CanonicalValue("arm_model")},
                })),
      scene_box("table_marker", CanonicalValue(CanonicalValue::Object{
                                    {"kind", CanonicalValue("cell_frame")},
                                })),
  });
  bool replaced = false;
  for (auto &[name, value] : members) {
    if (name == "scene") {
      value = scene;
      replaced = true;
    }
  }
  if (!replaced) members.emplace_back("scene", std::move(scene));
  const CanonicalValue root(std::move(members));
  const std::string revision =
      compute_content_identity(root, ContentIdentitySpec{
                                         .identity_domain = "program-revision",
                                         .api_version = "manyforge.program.v2",
                                         .own_field = "program_revision",
                                     });
  CanonicalValue::Object identified = root.as_object();
  for (auto &[name, value] : identified) {
    if (name == "program_revision") value = CanonicalValue(revision);
  }
  return canonical_json(CanonicalValue(std::move(identified)));
}

TEST(ProgramBindingCompilerTest,
     NonExecutableSceneRefusesObjectsOnUnresolvedFrames) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      program_with_scene(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto cell_id = context.bundle->value().cell_graph.cell_id;

  // The whole component is gone: its frame cannot be verified, so the object
  // is refused by name instead of being dropped from planning.
  auto component_removed = valid_assignment();
  component_removed.component_roles.front().controllable_component_id =
      manyforge::core::ControllableComponentId("removed_robot");
  const auto without_robot = compile_program_binding_outcome(
      *contract, component_removed, *context.bundle, context.model_map,
      catalog_store);
  ASSERT_TRUE(without_robot.resolved_roles.has_value());
  EXPECT_THROW(static_cast<void>(manyforge::deployment::project_resolved_initial_scene(
                   *contract, *without_robot.resolved_roles, cell_id)),
               manyforge::deployment::ProgramBindingError);
}

TEST(ProgramBindingCompilerTest,
     NonExecutableCapabilityDoesNotDiscardVerifiedSceneFrames) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  auto program = manyforge::deployment::parse_strict_json_model_yaml(
                     program_with_scene(*context.bundle)).root();
  const auto &targets = *program.find_object_member("target_roles");
  auto arm = *targets.find_object_member("arm");
  // A valid capability exists in the catalog but not on this arm target.
  // Its missing provider must block execution without invalidating tcp.
  arm = with_member(arm, "required_capabilities",
      CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
          {"capability_id", CanonicalValue("tool.gripper.command")},
          {"parameters", CanonicalValue(CanonicalValue::Object{})},
          {"disposition", CanonicalValue("required")},
      })}));
  program = with_member(program, "target_roles", with_member(targets, "arm", arm));
  program = with_member(program, "program_revision", CanonicalValue(
      compute_content_identity(program, ContentIdentitySpec{
          .identity_domain = "program-revision",
          .api_version = "manyforge.program.v2",
          .own_field = "program_revision"})));
  const auto contract = verify_program_binding_contract(
      canonical_json(program), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto outcome = compile_program_binding_outcome(
      *contract, valid_assignment(), *context.bundle, context.model_map,
      catalog_store);
  EXPECT_EQ(outcome.binding, nullptr);
  ASSERT_TRUE(outcome.non_executable.has_value());
  ASSERT_EQ(outcome.non_executable->unresolved_roles.size(), 1U);
  EXPECT_EQ(outcome.non_executable->unresolved_roles.front().reason,
            ProgramBindingUnresolvedReason::kCapabilityUnavailable);
  ASSERT_TRUE(outcome.resolved_roles.has_value());
  const auto scene = manyforge::deployment::project_resolved_initial_scene(
      *contract, *outcome.resolved_roles,
      context.bundle->value().cell_graph.cell_id);
  ASSERT_EQ(scene.size(), 2U);
  EXPECT_TRUE(std::holds_alternative<manyforge::core::ComponentSceneFrameRefDTO>(
      scene.front().frame_ref));
  auto endpoint_annotated_scene_roles = *outcome.resolved_roles;
  for (auto& target : endpoint_annotated_scene_roles.target_roles) {
    for (auto& frame : target.frame_role_bindings) {
      frame.contact_body_role_ids = {
          manyforge::core::CollisionBodyRoleId("endpoint_contact_body")};
    }
  }
  EXPECT_EQ(manyforge::deployment::project_resolved_initial_scene(
                *contract, endpoint_annotated_scene_roles,
                context.bundle->value().cell_graph.cell_id),
            scene);
  for (const auto &target : outcome.resolved_roles->target_roles) {
    EXPECT_TRUE(target.capability_bindings.empty());
  }
}

TEST(ProgramBindingCompilerTest,
     DistinctModeledEndpointsMayShareOneRetainedBodyGroup) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), false, true, true);
  auto program = make_program_root(*context.bundle);
  const auto &target_table = *program.find_object_member("target_roles");
  auto arm = *target_table.find_object_member("arm");
  arm = with_member(arm, "required_frame_roles",
                    CanonicalValue(CanonicalValue::Array{
                        CanonicalValue("base"), CanonicalValue("tcp")}));
  arm = with_member(arm, "required_collision_body_roles",
                    CanonicalValue(CanonicalValue::Array{}));
  program = with_member(program, "target_roles",
                        with_member(target_table, "arm", arm));
  program = with_member(
      program, "program_revision",
      CanonicalValue(compute_content_identity(
          program, ContentIdentitySpec{.identity_domain = "program-revision",
                                       .api_version = "manyforge.program.v2",
                                       .own_field = "program_revision"})));
  const auto contract =
      verify_program_binding_contract(canonical_json(program), *context.bundle);
  FilesystemCatalogArtifactStore catalog(root.path());
  const auto binding =
      compile_program_binding(*contract, valid_assignment(), *context.bundle,
                              context.model_map, catalog);
  ASSERT_NE(binding, nullptr);
  ASSERT_EQ(binding->value().target_role_bindings.size(), 1U);
  const auto &target = binding->value().target_role_bindings.front();
  ASSERT_EQ(target.frame_role_bindings.size(), 2U);
  const auto &first = target.frame_role_bindings.front();
  const auto &second = target.frame_role_bindings.back();
  EXPECT_EQ(first.frame_role_id, manyforge::core::FrameRoleId("base"));
  EXPECT_EQ(second.frame_role_id, manyforge::core::FrameRoleId("tcp"));
  EXPECT_NE(first.compiled_frame_name, second.compiled_frame_name);
  EXPECT_EQ(first.model_instance_id, second.model_instance_id);
  EXPECT_EQ(first.model_revision, second.model_revision);
  EXPECT_EQ(first.contact_body_role_ids, second.contact_body_role_ids);
  EXPECT_EQ(first.contact_body_role_ids,
            std::vector<manyforge::core::CollisionBodyRoleId>{
                manyforge::core::CollisionBodyRoleId("arm_collision")});
  // Neither Program requested bodies explicitly; both endpoints share exactly
  // one retained body row instead of duplicating or aliasing the frames.
  ASSERT_EQ(target.collision_body_role_bindings.size(), 1U);
  const auto &body = target.collision_body_role_bindings.front();
  EXPECT_EQ(body.collision_body_role_id, first.contact_body_role_ids.front());
  EXPECT_EQ(body.model_instance_id, first.model_instance_id);
  EXPECT_EQ(body.model_revision, first.model_revision);
  EXPECT_FALSE(body.compiled_collision_body_names.empty());
  EXPECT_EQ(manyforge::deployment::reload_verified_program_binding(
                binding->canonical_bytes(), *context.bundle, context.model_map)
                ->value(),
            binding->value());
}

TEST(ProgramBindingCompilerTest,
     RejectsUnknownRuntimeSelectorsAndCrossBundleContext) {
  ScopedTrustedRoot observed_root;
  const BindingCompilerContext observed = make_context(observed_root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*observed.bundle), *observed.bundle);
  FilesystemCatalogArtifactStore catalog_store(observed_root.path());
  const auto binding = compile_program_binding(
      *contract, valid_assignment(), *observed.bundle, observed.model_map,
      catalog_store);

  EXPECT_THROW(static_cast<void>(resolve_runtime_program_target(
                   *binding, TargetRoleId("missing"),
                   CapabilityId("state.observed.current"), *observed.bundle,
                   observed.model_map)),
               RuntimeProgramBindingResolutionError);
  EXPECT_THROW(
      static_cast<void>(resolve_runtime_program_target(
          *binding, TargetRoleId("arm"), CapabilityId("missing.capability"),
          *observed.bundle, observed.model_map)),
      RuntimeProgramBindingResolutionError);

  ScopedTrustedRoot other_root;
  const BindingCompilerContext other = make_ros_motion_context(other_root.path());
  EXPECT_THROW(
      static_cast<void>(resolve_runtime_program_target(
          *binding, TargetRoleId("arm"), CapabilityId("state.observed.current"),
          *other.bundle, other.model_map)),
      RuntimeProgramBindingResolutionError);
}

TEST(ProgramBindingCompilerTest,
     RejectsMissingRuntimeEpochAndNonDesiredI6Route) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto binding = compile_program_binding(
      *contract, valid_assignment(), *context.bundle, context.model_map,
      catalog_store);
  const auto target = resolve_runtime_program_target(
      *binding, TargetRoleId("arm"), CapabilityId("state.observed.current"),
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
    const RetainedBundle &bundle, bool mixed = false,
    std::string_view output_role = "lamp",
    std::string_view output_capability = "io.gpio.set_output") {
  auto root = make_program_root(bundle);
  root =
      with_member(root, "component_roles",
                  CanonicalValue(CanonicalValue::Object{
                      {"device", CanonicalValue(CanonicalValue::Object{})}}));
  CanonicalValue::Array capabilities;
  for (const auto &id :
       mixed ? std::vector<std::string>{"io.gpio.get_input",
                                        std::string(output_capability)}
             : std::vector<std::string>{std::string(output_capability)})
    capabilities.emplace_back(CanonicalValue::Object{
        {"capability_id", CanonicalValue(id)},
        {"parameters", CanonicalValue(CanonicalValue::Object{})},
        {"disposition", CanonicalValue("required")}});
  CanonicalValue::Array roles{CanonicalValue(std::string(output_role))};
  if (mixed && output_role != "ready")
    roles.emplace_back("ready");
  root = with_member(
      root, "target_roles",
      CanonicalValue(CanonicalValue::Object{
          {"signals",
           CanonicalValue(CanonicalValue::Object{
               {"component_role", CanonicalValue("device")},
               {"required_capabilities",
                CanonicalValue(std::move(capabilities))},
               {"required_frame_roles",
                CanonicalValue(CanonicalValue::Array{})},
               {"required_resource_roles", CanonicalValue(std::move(roles))},
               {"required_collision_body_roles",
                CanonicalValue(CanonicalValue::Array{})},
               {"required_model_roles",
                CanonicalValue(CanonicalValue::Array{})}})}}));
  CanonicalValue::Array children;
  if (mixed)
    children.emplace_back(CanonicalValue::Object{
        {"node_id", CanonicalValue("io.check_input")},
        {"instance_id", CanonicalValue("check")},
        {"parameters", CanonicalValue(CanonicalValue::Object{
                           {"target_role", CanonicalValue("signals")},
                           {"resource_role", CanonicalValue("ready")},
                           {"expected_value", CanonicalValue(true)}})},
        {"children", CanonicalValue(CanonicalValue::Array{})}});
  children.emplace_back(CanonicalValue::Object{
      {"node_id", CanonicalValue("io.set_output")},
      {"instance_id", CanonicalValue("write")},
      {"parameters",
       CanonicalValue(CanonicalValue::Object{
           {"target_role", CanonicalValue("signals")},
           {"resource_role", CanonicalValue(std::string(output_role))},
           {"value", CanonicalValue(true)}})},
      {"children", CanonicalValue(CanonicalValue::Array{})}});
  root =
      with_member(root, "tree",
                  CanonicalValue(CanonicalValue::Object{
                      {"node_id", CanonicalValue("sequence")},
                      {"instance_id", CanonicalValue("root")},
                      {"parameters", CanonicalValue(CanonicalValue::Object{})},
                      {"children", CanonicalValue(std::move(children))}}));
  root = manyforge::deployment::normalize_program_authoring(root);
  root = with_member(
      root, "program_revision",
      CanonicalValue(compute_content_identity(
          root, ContentIdentitySpec{"program-revision", "manyforge.program.v2",
                                    "program_revision"})));
  return canonical_json(root);
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
        output_binding_program(bundle, mixed), bundle);
    const ProgramRoleAssignmentDTO assignment{
        {{ComponentRoleId("device"),
          manyforge::core::ControllableComponentId("device.io")}},
        {{TargetRoleId("signals"), manyforge::core::CommandTargetId("gpio")}}};
    const auto binding =
        compile_program_binding(*program, assignment, bundle, *map, catalog);
    ASSERT_NE(binding, nullptr);
    const auto &target = binding->value().target_role_bindings.front();
    EXPECT_TRUE(target.resource_role_bindings.empty());
    ASSERT_EQ(target.signal_role_bindings.size(), mixed ? 2U : 1U);
    const auto output =
        std::find_if(target.signal_role_bindings.begin(),
                     target.signal_role_bindings.end(), [](const auto &role) {
                       return role.resource_role_id.value() == "lamp";
                     });
    ASSERT_NE(output, target.signal_role_bindings.end());
    EXPECT_EQ(output->direction, manyforge::core::IoSignalDirection::kOutput);
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
                                              bundle, *map)
                  ->value(),
              binding->value());
    const auto direct =
        compile_direct_control_program_binding_inventory(bundle, *map, catalog);
    ASSERT_EQ(direct.io.size(), 1U);
    EXPECT_TRUE(direct.motion.empty());
    EXPECT_TRUE(direct.gripper.empty());
    const auto &direct_target =
        direct.io.front()->value().target_role_bindings.front();
    EXPECT_EQ(direct_target.signal_role_bindings.size(), mixed ? 2U : 1U);
    EXPECT_EQ(direct_target.capability_bindings.size(), mixed ? 2U : 1U);
    EXPECT_TRUE(direct_target.resource_role_bindings.empty());
    EXPECT_EQ(reload_verified_program_binding(
                  direct.io.front()->canonical_bytes(), bundle, *map)
                  ->value(),
              direct.io.front()->value());
  }
}

TEST(ProgramBindingCompilerTest,
     OutputRefusesInputRoleAndWrongRequiredCapability) {
  ScopedTrustedRoot root;
  const auto fixture =
      manyforge::deployment::test::materialize_boolean_io_retained_fixture(
          root.path(), true, true);
  const auto &bundle = *fixture.bundle;
  FilesystemVerifiedRetainedModelMapSource maps(root.path());
  const auto map = maps.get_verified(
      {bundle.retained_bundle_id(),
       bundle.value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(map);
  FilesystemCatalogArtifactStore catalog(root.path());
  const ProgramRoleAssignmentDTO assignment{
      {{ComponentRoleId("device"),
        manyforge::core::ControllableComponentId("device.io")}},
      {{TargetRoleId("signals"), manyforge::core::CommandTargetId("gpio")}}};
  const auto wrong_direction = verify_program_binding_contract(
      output_binding_program(bundle, false, "ready"), bundle);
  EXPECT_THROW(static_cast<void>(compile_program_binding(
                   *wrong_direction, assignment, bundle, *map, catalog)),
               ProgramBindingCompilerError);
  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          output_binding_program(bundle, false, "lamp", "io.gpio.get_input"),
          bundle)),
      manyforge::deployment::ProgramContractError);
}

} // namespace

TEST(ProgramBindingCompilerTest, PreparedOriginIsClosedAndScenarioAccessIsChecked) {
  namespace d = manyforge::deployment;
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto prepared = prepare(root.path(), context, kLite6ArmTargetId);
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
  const BindingCompilerContext context = make_context(root.path());
  const auto first = prepare(root.path(), context, kLite6ArmTargetId,
                             "/fixtures/first/same.scenario");
  const auto second = prepare(root.path(), context, kLite6ArmTargetId,
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
                               std::string_view edge = "level") {
  auto root = make_program_root(bundle);
  root =
      with_member(root, "component_roles",
                  CanonicalValue(CanonicalValue::Object{
                      {"device", CanonicalValue(CanonicalValue::Object{})}}));
  CanonicalValue::Array capabilities;
  for (const auto *id : {"io.gpio.get_input", "io.gpio.wait_for_input"}) {
    capabilities.emplace_back(CanonicalValue::Object{
        {"capability_id", CanonicalValue(id)},
        {"parameters", CanonicalValue(CanonicalValue::Object{})},
        {"disposition", CanonicalValue("required")}});
  }
  root = with_member(
      root, "target_roles",
      CanonicalValue(CanonicalValue::Object{
          {"inputs",
           CanonicalValue(CanonicalValue::Object{
               {"component_role", CanonicalValue("device")},
               {"required_capabilities",
                CanonicalValue(std::move(capabilities))},
               {"required_frame_roles",
                CanonicalValue(CanonicalValue::Array{})},
               {"required_resource_roles",
                CanonicalValue(CanonicalValue::Array{CanonicalValue("ready")})},
               {"required_collision_body_roles",
                CanonicalValue(CanonicalValue::Array{})},
               {"required_model_roles",
                CanonicalValue(CanonicalValue::Array{})}})}}));
  CanonicalValue::Array children;
  for (const bool wait : {false, true}) {
    CanonicalValue::Object parameters{
        {"target_role", CanonicalValue("inputs")},
        {"resource_role", CanonicalValue("ready")},
        {"expected_value", CanonicalValue(true)}};
    if (wait)
      parameters.emplace_back("edge", CanonicalValue(std::string(edge)));
    children.emplace_back(CanonicalValue::Object{
        {"node_id",
         CanonicalValue(wait ? "io.wait_for_input" : "io.check_input")},
        {"instance_id", CanonicalValue(wait ? "wait" : "check")},
        {"parameters", CanonicalValue(std::move(parameters))},
        {"children", CanonicalValue(CanonicalValue::Array{})}});
  }
  root =
      with_member(root, "tree",
                  CanonicalValue(CanonicalValue::Object{
                      {"node_id", CanonicalValue("sequence")},
                      {"instance_id", CanonicalValue("root")},
                      {"parameters", CanonicalValue(CanonicalValue::Object{})},
                      {"children", CanonicalValue(std::move(children))}}));
  root = with_member(
      root, "program_revision",
      CanonicalValue(compute_content_identity(
          root, ContentIdentitySpec{"program-revision", "manyforge.program.v2",
                                    "program_revision"})));
  return canonical_json(root);
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
  const auto program =
      verify_program_binding_contract(io_binding_program(bundle), bundle);
  const ProgramRoleAssignmentDTO assignment{
      {{ComponentRoleId("device"),
        manyforge::core::ControllableComponentId("device.io")}},
      {{TargetRoleId("inputs"), manyforge::core::CommandTargetId("gpio")}}};
  FilesystemCatalogArtifactStore catalog(root.path());
  const auto binding =
      compile_program_binding(*program, assignment, bundle, *map, catalog);
  ASSERT_NE(binding, nullptr);
  const auto direct =
      compile_direct_control_program_binding_inventory(bundle, *map, catalog);
  EXPECT_TRUE(direct.motion.empty());
  EXPECT_TRUE(direct.gripper.empty());
  ASSERT_EQ(direct.io.size(), 1U);
  EXPECT_EQ(direct.io.front()->value().program_id.value(),
            manyforge::deployment::kDirectControlIoProgramId);
  ASSERT_EQ(direct.io.front()->value().target_role_bindings.size(), 1U);
  EXPECT_TRUE(direct.io.front()
                  ->value()
                  .target_role_bindings.front()
                  .resource_role_bindings.empty());
  EXPECT_EQ(direct.io.front()
                ->value()
                .target_role_bindings.front()
                .signal_role_bindings.size(),
            1U);
  EXPECT_EQ(reload_verified_program_binding(
                direct.io.front()->canonical_bytes(), bundle, *map)
                ->value(),
            direct.io.front()->value());
  ASSERT_EQ(binding->value().target_role_bindings.size(), 1U);
  const auto &target = binding->value().target_role_bindings.front();
  EXPECT_TRUE(target.resource_role_bindings.empty());
  ASSERT_EQ(target.signal_role_bindings.size(), 1U);
  EXPECT_EQ(target.signal_role_bindings.front().resource_role_id.value(),
            "ready");
  EXPECT_EQ(target.signal_role_bindings.front().signal_name, "ready");
  ASSERT_EQ(target.capability_bindings.size(), 2U);
  EXPECT_TRUE(target.capability_bindings.front().capability_provider_binding);
  EXPECT_FALSE(target.capability_bindings.front().control_binding);
  const auto reopened = manyforge::deployment::reload_verified_program_binding(
      binding->canonical_bytes(), bundle, *map);
  EXPECT_EQ(reopened->value(), binding->value());
  auto forged = binding->value();
  forged.target_role_bindings.front()
      .signal_role_bindings.front()
      .signal_descriptor_digest =
      manyforge::core::ContentDigest(std::string(kZeroDigest));
  const auto resealed =
      manyforge::deployment::detail::ProgramBindingConstructionAuthority::seal(
          forged);
  EXPECT_THROW(
      static_cast<void>(manyforge::deployment::reload_verified_program_binding(
          resealed->canonical_bytes(), bundle, *map)),
      manyforge::deployment::ProgramBindingError);
  const auto edge_program = verify_program_binding_contract(
      io_binding_program(bundle, "rising"), bundle);
  EXPECT_THROW(static_cast<void>(compile_program_binding(
                   *edge_program, assignment, bundle, *map, catalog)),
               ProgramBindingCompilerError);
}

TEST(ProgramBindingCompilerTest, CompilesPortableReleaseWithoutChangingItsEnvelope) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), true);
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
                                   .api_version = "manyforge.program.v2",
                                   .own_field = "program_revision"})));
  const auto contract = verify_program_binding_contract(canonical_json(program), *context.bundle);
  ASSERT_EQ(contract->value().contact_grasp_templates.size(), 1U);
  const auto& release = std::get<manyforge::core::ContactReleaseEnvelopeDTO>(
      contract->value().contact_grasp_templates.front().envelope);
  EXPECT_DOUBLE_EQ(release.max_start_penetration_m, 0.002);
  EXPECT_TRUE(release.require_separation_by_end);
  EXPECT_TRUE(release.require_strictly_decreasing_penetration);
  EXPECT_FALSE(release.allow_recontact);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto binding = compile_program_binding(
      *contract, gripper_assignment(), *context.bundle, context.model_map, catalog_store);
  ASSERT_EQ(binding->value().contact_grasp_bindings.size(), 1U);
  const auto& bound = binding->value().contact_grasp_bindings.front();
  EXPECT_EQ(bound.envelope, contract->value().contact_grasp_templates.front().envelope);
  EXPECT_EQ(bound.runtime_object_id.value(), "selected_object");
  ASSERT_EQ(bound.allowed_body_bindings.size(), 1U);
  EXPECT_EQ(bound.allowed_body_bindings.front().target_role_id, TargetRoleId("gripper"));
  const auto& body = bound.allowed_body_bindings.front();
  EXPECT_EQ(body.collision_body_role_id.value(), "gripper_collision");
  ASSERT_FALSE(body.collision_body_ids.empty());
  for (std::size_t index = 0; index < body.collision_body_ids.size(); ++index)
    EXPECT_EQ(body.collision_body_ids[index], manyforge::core::make_component_collision_body_id(
        body.controllable_component_id, body.model_instance_id, body.collision_body_role_id, index));
  EXPECT_EQ(manyforge::deployment::reload_verified_program_binding(
                binding->canonical_bytes(), *context.bundle, context.model_map)->value(),
            binding->value());
}

using GripperRead = std::pair<std::string_view, std::string_view>;

// The gripper Program whose gripper role also declares the
// given input capabilities (sorted, before `tool.gripper.command`) and the
// `carried_object_lost` system resource role, read by the given
// (node_id, resource_role) input nodes. The Lite6 gripper target offers no
// input capability (contract §8.3, SX-269).
[[nodiscard]] std::string gripper_input_program(
    const RetainedBundle &bundle, const std::vector<std::string_view> &input_capabilities,
    const std::vector<GripperRead> &reads) {
  auto root = make_gripper_program_root(bundle);
  auto targets = root.find_object_member("target_roles")->as_object();
  for (auto &[name, value] : targets) {
    if (name != "gripper") continue;
    CanonicalValue::Array capabilities;
    for (const auto id : input_capabilities)
      capabilities.emplace_back(CanonicalValue::Object{
          {"capability_id", CanonicalValue(std::string(id))},
          {"parameters", CanonicalValue(CanonicalValue::Object{})},
          {"disposition", CanonicalValue("required")}});
    for (const auto &existing :
         value.find_object_member("required_capabilities")->as_array())
      capabilities.push_back(existing);
    value = with_member(value, "required_capabilities",
                        CanonicalValue(std::move(capabilities)));
    value = with_member(value, "required_resource_roles",
                        CanonicalValue(CanonicalValue::Array{
                            CanonicalValue("carried_object_lost"),
                            CanonicalValue("fingers")}));
  }
  root = with_member(root, "target_roles", CanonicalValue(std::move(targets)));
  CanonicalValue::Array children;
  for (const auto &[node_id, resource] : reads)
    children.emplace_back(CanonicalValue::Object{
        {"node_id", CanonicalValue(std::string(node_id))},
        {"instance_id", CanonicalValue("read_" + std::to_string(children.size()))},
        {"parameters", CanonicalValue(CanonicalValue::Object{
                           {"target_role", CanonicalValue("gripper")},
                           {"resource_role", CanonicalValue(std::string(resource))},
                           {"expected_value", CanonicalValue(true)}})},
        {"children", CanonicalValue(CanonicalValue::Array{})}});
  root = with_member(root, "tree",
                     CanonicalValue(CanonicalValue::Object{
                         {"node_id", CanonicalValue("sequence")},
                         {"instance_id", CanonicalValue("root")},
                         {"parameters", CanonicalValue(CanonicalValue::Object{})},
                         {"children", CanonicalValue(std::move(children))}}));
  root = manyforge::deployment::normalize_program_authoring(root);
  root = with_member(root, "program_revision",
                     CanonicalValue(compute_content_identity(
                         root, ContentIdentitySpec{"program-revision", "manyforge.program.v2",
                                                   "program_revision"})));
  return canonical_json(root);
}

TEST(ProgramBindingCompilerTest,
     InputReadOfCompiledSystemSignalBindsWithoutTargetInputCapability) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), true);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const std::vector<std::pair<std::string_view, GripperRead>> cases{
      {"io.gpio.get_input", {"io.check_input", "carried_object_lost"}},
      {"io.gpio.wait_for_input", {"io.wait_for_input", "carried_object_lost"}}};
  for (const auto &row : cases) {
    const std::string_view capability = row.first;
    const auto contract = verify_program_binding_contract(
        gripper_input_program(*context.bundle, {capability}, {row.second}),
        *context.bundle);
    const auto binding = compile_program_binding(
        *contract, gripper_assignment(), *context.bundle, context.model_map, catalog_store);
    ASSERT_NE(binding, nullptr);
    const auto &targets = binding->value().target_role_bindings;
    const auto gripper = std::find_if(targets.begin(), targets.end(), [](const auto &row) {
      return row.target_role_id == TargetRoleId("gripper");
    });
    ASSERT_NE(gripper, targets.end());
    ASSERT_EQ(gripper->signal_role_bindings.size(), 1U);
    const auto &signal = gripper->signal_role_bindings.front();
    EXPECT_EQ(signal.resource_role_id.value(), "carried_object_lost");
    EXPECT_EQ(signal.signal_name,
              "carried_object_lost." + std::string(kLite6GripperTargetId));
    EXPECT_EQ(signal.direction, manyforge::core::IoSignalDirection::kInput);
    // The system signal serves the read; no input capability is invented.
    EXPECT_TRUE(std::none_of(
        gripper->capability_bindings.begin(), gripper->capability_bindings.end(),
        [&](const auto &row) { return row.capability_id.value() == capability; }));
    EXPECT_EQ(manyforge::deployment::reload_verified_program_binding(
                  binding->canonical_bytes(), *context.bundle, context.model_map)->value(),
              binding->value());
  }
}

TEST(ProgramBindingCompilerTest,
     OrdinaryInputReadOnGripperTargetIsStillRefusedAlsoBesideTheSystemSignal) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), true);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  // Ordinary read alone, then mixed with the system read on the same role:
  // the exemption follows each operand, never the whole target role.
  const std::vector<std::vector<GripperRead>> cases{
      {{"io.check_input", "fingers"}},
      {{"io.check_input", "carried_object_lost"}, {"io.check_input", "fingers"}}};
  for (const auto &reads : cases) {
    const auto contract = verify_program_binding_contract(
        gripper_input_program(*context.bundle, {"io.gpio.get_input"}, reads),
        *context.bundle);
    const auto outcome = compile_program_binding_outcome(
        *contract, gripper_assignment(), *context.bundle, context.model_map, catalog_store);
    EXPECT_EQ(outcome.binding, nullptr);
    ASSERT_TRUE(outcome.non_executable.has_value());
    const auto &rows = outcome.non_executable->unresolved_roles;
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.front().target_role_id, TargetRoleId("gripper"));
    EXPECT_EQ(rows.front().reason, ProgramBindingUnresolvedReason::kTargetIncompatible);
    EXPECT_THROW(static_cast<void>(compile_program_binding(
                     *contract, gripper_assignment(), *context.bundle,
                     context.model_map, catalog_store)),
                 ProgramBindingCompilerError);
  }
}

TEST(ProgramBindingCompilerTest,
     InputCapabilityWithoutItsOwnSystemSignalReadIsStillRefused) {
  ScopedTrustedRoot root;
  const auto context = make_context(root.path(), true);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  // The system signal is read by Wait Input only. `io.gpio.get_input` has no
  // read requirement of its own on the role, so the target must offer it.
  const auto contract = verify_program_binding_contract(
      gripper_input_program(*context.bundle,
                            {"io.gpio.get_input", "io.gpio.wait_for_input"},
                            {{"io.wait_for_input", "carried_object_lost"}}),
      *context.bundle);
  const auto outcome = compile_program_binding_outcome(
      *contract, gripper_assignment(), *context.bundle, context.model_map, catalog_store);
  EXPECT_EQ(outcome.binding, nullptr);
  ASSERT_TRUE(outcome.non_executable.has_value());
  const auto &rows = outcome.non_executable->unresolved_roles;
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().target_role_id, TargetRoleId("gripper"));
  EXPECT_EQ(rows.front().reason, ProgramBindingUnresolvedReason::kCapabilityUnavailable);
}

} // namespace
