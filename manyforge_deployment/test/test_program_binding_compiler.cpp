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
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "content_identity_internal.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "lite6_retained_consumer_fixture.hpp"

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
using manyforge::deployment::test::
    materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture;
using manyforge::deployment::test::
    materialize_lite6_native_motion_retained_consumer_fixture;
using manyforge::deployment::test::materialize_lite6_retained_consumer_fixture;

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
make_context(const std::filesystem::path &root,
             bool aggregate_gripper = false) {
  const auto materialized =
      aggregate_gripper
          ? materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
                root)
          : materialize_lite6_retained_consumer_fixture(root);
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
make_native_motion_context(const std::filesystem::path &root) {
  const auto materialized =
      materialize_lite6_native_motion_retained_consumer_fixture(root);
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
                  bool include_node_policy = false) {
  CanonicalValue::Object tree{
      {"node_id", CanonicalValue("sequence")},
      {"instance_id", CanonicalValue("root")},
      {"parameters", CanonicalValue(CanonicalValue::Object{})},
      {"children", CanonicalValue(CanonicalValue::Array{})},
  };
  if (include_node_policy) {
    tree.emplace_back(
        "modification_policy",
        CanonicalValue(CanonicalValue::Object{
            {"runtime_override", CanonicalValue("human_approval")},
            {"persistent_edit", CanonicalValue("forbidden")},
            {"intervention_delegate", CanonicalValue("human_only")},
        }));
    tree.emplace_back(
        "recovery_policy",
        CanonicalValue(CanonicalValue::Object{
            {"allow_intervention", CanonicalValue(false)},
            {"allowed_recovery_actions",
             CanonicalValue(
                 CanonicalValue::Array{CanonicalValue("runtime_override")})},
            {"max_retries", CanonicalValue(1.0)},
            {"allowed_skills", CanonicalValue(CanonicalValue::Array{})},
            {"persistence_ceiling", CanonicalValue("session_only")},
        }));
    tree.emplace_back("operator_stop_policy", CanonicalValue("hold"));
  }
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
                {"required_capabilities",
                 CanonicalValue(CanonicalValue::Array{
                     CanonicalValue(CanonicalValue::Object{
                         {"capability_id",
                          CanonicalValue("state.observed.current")},
                         {"parameters",
                          CanonicalValue(CanonicalValue::Object{})},
                         {"disposition", CanonicalValue("required")},
                     })})},
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

[[nodiscard]] CanonicalValue
make_aggregate_program_root(const RetainedBundle &bundle) {
  CanonicalValue::Object root = make_program_root(bundle).as_object();
  for (auto &[name, value] : root) {
    if (name == "component_roles") {
      value = CanonicalValue(CanonicalValue::Object{
          {"robot",
           CanonicalValue(CanonicalValue::Object{
               {"required_component_type_tags",
                CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("articulated-arm"),
                    CanonicalValue("articulated-gripper")})},
               {"required_model_roles", CanonicalValue(CanonicalValue::Array{
                                            CanonicalValue("arm_model")})},
           })},
      });
    } else if (name == "target_roles") {
      CanonicalValue::Object targets = value.as_object();
      targets.emplace_back(
          "gripper",
          CanonicalValue(CanonicalValue::Object{
              {"component_role", CanonicalValue("robot")},
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
                                           CanonicalValue("arm_model")})},
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
                             {"component_role_id", CanonicalValue("robot")},
                             {"model_role_id", CanonicalValue("arm_model")},
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

[[nodiscard]] std::string make_program_bytes(const RetainedBundle &bundle,
                                             bool aggregate_gripper = false,
                                             bool include_node_policy = false) {
  CanonicalValue root = aggregate_gripper
                            ? make_aggregate_program_root(bundle)
                            : make_program_root(bundle, include_node_policy);
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

[[nodiscard]] ProgramRoleAssignmentDTO
valid_assignment(bool aggregate_gripper = false) {
  ProgramRoleAssignmentDTO result{
      {ProgramComponentRoleAssignmentDTO{
          ComponentRoleId("robot"),
          manyforge::core::ControllableComponentId(
              std::string(kLite6ControllableComponentId))}},
      {ProgramTargetRoleAssignmentDTO{
          TargetRoleId("arm"),
          manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId))}},
  };
  if (aggregate_gripper) {
    result.target_roles.push_back(ProgramTargetRoleAssignmentDTO{
        TargetRoleId("gripper"),
        manyforge::core::CommandTargetId(std::string(kLite6GripperTargetId))});
  }
  return result;
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
     CopiesVerifiedNodePoliciesAndLeavesDirectControlEmpty) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path());
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle, false, true), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto binding =
      compile_program_binding(*contract, valid_assignment(), *context.bundle,
                              context.model_map, catalog_store);

  ASSERT_EQ(contract->value().node_policies.size(), 1U);
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(binding->value().node_policies, contract->value().node_policies);

  ScopedTrustedRoot direct_root;
  const BindingCompilerContext direct_context =
      make_native_motion_context(direct_root.path());
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
      make_native_motion_context(root.path());
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
  EXPECT_NE(
      std::find_if(target.frame_role_bindings.begin(),
                   target.frame_role_bindings.end(),
                   [](const auto &frame) {
                     return frame.frame_role_id.value() ==
                            manyforge::deployment::kDirectControlTcpFrameRoleId;
                   }),
      target.frame_role_bindings.end());
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

TEST(ProgramBindingCompilerTest,
     DirectControlMotionBindingRejectsUnknownComponentOrTarget) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context =
      make_native_motion_context(root.path());
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
          std::string(kLite6ControllableComponentId)),
      manyforge::core::CommandTargetId(std::string(kLite6GripperTargetId)),
      *context.bundle, context.model_map, catalog_store);
  const auto second = compile_direct_control_gripper_program_binding(
      manyforge::core::ControllableComponentId(
          std::string(kLite6ControllableComponentId)),
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
  const BindingCompilerContext context = make_context(root.path(), true);
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
      make_native_motion_context(motion_root.path());
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
prepare(const std::filesystem::path &root, const BindingCompilerContext &context,
        std::string_view command_target) {
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root);
  manyforge::deployment::FilesystemRawArtifactStore raw_store(root);
  return manyforge::deployment::assemble_prepared_activation(
      {manyforge::core::ContentDigest("sha256:" + std::string(64U, '1')),
       "scenario.readback",
       manyforge::core::ContentDigest("sha256:" + std::string(64U, '2')),
       "optional", CanonicalValue(CanonicalValue::Object{})},
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

/// The aggregate arm+gripper Program with an authored initial Scene: one
/// object on the robot's arm-model `tcp` frame and one in the cell frame.
[[nodiscard]] std::string aggregate_program_with_scene(const RetainedBundle &bundle) {
  CanonicalValue::Object members = make_aggregate_program_root(bundle).as_object();
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
     NonExecutableSceneKeepsObjectsOnSurvivingFramesAndRefusesUnresolvedOnes) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path(), true);
  const auto contract = verify_program_binding_contract(
      aggregate_program_with_scene(*context.bundle), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto cell_id = context.bundle->value().cell_graph.cell_id;

  // The gripper target is gone; the arm and its tcp frame still resolve.
  auto gripper_removed = valid_assignment(true);
  gripper_removed.target_roles.back().command_target_id =
      manyforge::core::CommandTargetId("removed_gripper_target");
  const auto without_gripper = compile_program_binding_outcome(
      *contract, gripper_removed, *context.bundle, context.model_map,
      catalog_store);
  ASSERT_TRUE(without_gripper.non_executable.has_value());
  ASSERT_TRUE(without_gripper.resolved_roles.has_value());
  const auto scene = manyforge::deployment::project_resolved_initial_scene(
      *contract, *without_gripper.resolved_roles, cell_id);
  ASSERT_EQ(scene.size(), 2U);
  const auto *arm_frame =
      std::get_if<manyforge::core::ComponentSceneFrameRefDTO>(&scene.front().frame_ref);
  ASSERT_NE(arm_frame, nullptr);
  EXPECT_EQ(arm_frame->controllable_component_id.value(),
            kLite6ControllableComponentId);

  // The whole component is gone: its frame cannot be verified, so the object
  // is refused by name instead of being dropped from planning.
  auto component_removed = valid_assignment(true);
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
  const BindingCompilerContext context = make_context(root.path(), true);
  auto program = manyforge::deployment::parse_strict_json_model_yaml(
                     aggregate_program_with_scene(*context.bundle)).root();
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
      *contract, valid_assignment(true), *context.bundle, context.model_map,
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
  for (const auto &target : outcome.resolved_roles->target_roles) {
    EXPECT_TRUE(target.capability_bindings.empty());
  }
}

TEST(ProgramBindingCompilerTest,
     BindsOneAggregateComponentToDistinctArmAndPrismaticGripperTargets) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path(), true);
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle, true), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());

  const auto binding = compile_program_binding(
      *contract, valid_assignment(true), *context.bundle, context.model_map,
      catalog_store);

  ASSERT_NE(binding, nullptr);
  ASSERT_EQ(context.bundle->value().cell_graph.controllable_components.size(),
            1U);
  ASSERT_EQ(binding->value().component_role_bindings.size(), 1U);
  ASSERT_EQ(binding->value().target_role_bindings.size(), 2U);
  const auto gripper = std::find_if(
      binding->value().target_role_bindings.begin(),
      binding->value().target_role_bindings.end(), [](const auto &value) {
        return value.target_role_id == TargetRoleId("gripper");
      });
  ASSERT_NE(gripper, binding->value().target_role_bindings.end());
  EXPECT_EQ(gripper->command_target_id.value(), kLite6GripperTargetId);
  ASSERT_EQ(gripper->resource_role_bindings.size(), 1U);
  EXPECT_EQ(gripper->resource_role_bindings.front().resource_ids.size(), 1U);
  ASSERT_EQ(gripper->capability_bindings.size(), 1U);
  const auto &capability = gripper->capability_bindings.front();
  EXPECT_EQ(capability.capability_id.value(), "tool.gripper.command");
  ASSERT_TRUE(capability.command_schema.has_value());
  EXPECT_EQ(capability.command_schema->command_schema_id.value(),
            manyforge::core::kGripperPositionCommandSchemaId);
  ASSERT_EQ(binding->value().contact_grasp_bindings.size(), 1U);
  const auto &contact = binding->value().contact_grasp_bindings.front();
  EXPECT_EQ(contact.contact_policy_id.value(), "selected_object_grasp");
  EXPECT_EQ(contact.runtime_object_id.value(), "selected_object");
  ASSERT_EQ(contact.allowed_body_bindings.size(), 1U);
  const auto &body = contact.allowed_body_bindings.front();
  EXPECT_EQ(body.target_role_id, TargetRoleId("gripper"));
  EXPECT_EQ(body.command_target_id.value(), kLite6GripperTargetId);
  EXPECT_EQ(body.collision_body_role_id.value(), "gripper_collision");
  ASSERT_FALSE(body.collision_body_ids.empty());
  EXPECT_EQ(body.collision_body_ids.front(),
            manyforge::core::make_component_collision_body_id(
                body.controllable_component_id, body.model_instance_id,
                body.collision_body_role_id, 0U));
  EXPECT_EQ(capability.command_schema->command_schema_revision.value(),
            manyforge::core::kGripperPositionCommandSchemaRevision);

  const auto *coordinate_map = context.model_map.find_binding(
      manyforge::core::ControllableComponentId(
          std::string(kLite6ControllableComponentId)),
      manyforge::core::CommandTargetId(std::string(kLite6GripperTargetId)));
  ASSERT_NE(coordinate_map, nullptr);
  ASSERT_EQ(coordinate_map->resources.size(), 1U);
  ASSERT_FALSE(coordinate_map->resources.front().coordinates.empty());
  EXPECT_EQ(
      coordinate_map->resources.front().coordinates.front().coordinate_kind,
      manyforge::deployment::UrdfCoordinateKind::kLinear);
  EXPECT_EQ(coordinate_map->resources.front().coordinates.front().unit,
            manyforge::deployment::CompositionCoordinateUnit::kMeters);
}

TEST(ProgramBindingCompilerTest,
     SealsExactRuntimeArmAndGripperTargetsFromOneAggregateComponent) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path(), true);
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle, true), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto binding = compile_program_binding(
      *contract, valid_assignment(true), *context.bundle, context.model_map,
      catalog_store);

  const auto arm = resolve_runtime_program_target(
      *binding, TargetRoleId("arm"), CapabilityId("state.observed.current"),
      *context.bundle, context.model_map);
  const auto gripper = resolve_runtime_program_target(
      *binding, TargetRoleId("gripper"), CapabilityId("tool.gripper.command"),
      *context.bundle, context.model_map);

  EXPECT_EQ(arm.program_binding_id(), binding->program_binding_id());
  EXPECT_EQ(gripper.program_binding_id(), binding->program_binding_id());
  EXPECT_EQ(arm.component().controllable_component_id,
            gripper.component().controllable_component_id);
  EXPECT_EQ(arm.target_role().command_target_id.value(), kLite6ArmTargetId);
  EXPECT_EQ(gripper.target_role().command_target_id.value(),
            kLite6GripperTargetId);

  ASSERT_TRUE(arm.control_binding().has_value());
  ASSERT_TRUE(gripper.control_binding().has_value());
  EXPECT_NE(arm.control_binding()->control_binding_id,
            gripper.control_binding()->control_binding_id);
  ASSERT_TRUE(arm.control_domain().has_value());
  ASSERT_TRUE(gripper.control_domain().has_value());
  EXPECT_NE(arm.control_domain()->control_domain_id,
            gripper.control_domain()->control_domain_id);
  ASSERT_TRUE(arm.desired_control_configuration().has_value());
  ASSERT_TRUE(gripper.desired_control_configuration().has_value());
  ASSERT_TRUE(arm.desired_route().has_value());
  ASSERT_TRUE(gripper.desired_route().has_value());

  ASSERT_TRUE(arm.coordinate_map().has_value());
  ASSERT_TRUE(gripper.coordinate_map().has_value());
  EXPECT_EQ(arm.coordinate_map()->resources.size(), 6U);
  ASSERT_EQ(gripper.coordinate_map()->resources.size(), 1U);
  ASSERT_FALSE(gripper.coordinate_map()->resources.front().coordinates.empty());
  EXPECT_EQ(gripper.coordinate_map()
                ->resources.front()
                .coordinates.front()
                .coordinate_kind,
            manyforge::deployment::UrdfCoordinateKind::kLinear);
  EXPECT_EQ(
      gripper.coordinate_map()->resources.front().coordinates.front().unit,
      manyforge::deployment::CompositionCoordinateUnit::kMeters);
  ASSERT_TRUE(gripper.capability().command_schema.has_value());
  EXPECT_EQ(gripper.capability().command_schema->command_schema_id.value(),
            manyforge::core::kGripperPositionCommandSchemaId);
}

TEST(ProgramBindingCompilerTest,
     RejectsUnknownRuntimeSelectorsAndCrossBundleContext) {
  ScopedTrustedRoot aggregate_root;
  const BindingCompilerContext aggregate =
      make_context(aggregate_root.path(), true);
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*aggregate.bundle, true), *aggregate.bundle);
  FilesystemCatalogArtifactStore catalog_store(aggregate_root.path());
  const auto binding = compile_program_binding(
      *contract, valid_assignment(true), *aggregate.bundle, aggregate.model_map,
      catalog_store);

  EXPECT_THROW(static_cast<void>(resolve_runtime_program_target(
                   *binding, TargetRoleId("missing"),
                   CapabilityId("state.observed.current"), *aggregate.bundle,
                   aggregate.model_map)),
               RuntimeProgramBindingResolutionError);
  EXPECT_THROW(
      static_cast<void>(resolve_runtime_program_target(
          *binding, TargetRoleId("arm"), CapabilityId("missing.capability"),
          *aggregate.bundle, aggregate.model_map)),
      RuntimeProgramBindingResolutionError);

  ScopedTrustedRoot other_root;
  const BindingCompilerContext other = make_context(other_root.path());
  EXPECT_THROW(
      static_cast<void>(resolve_runtime_program_target(
          *binding, TargetRoleId("arm"), CapabilityId("state.observed.current"),
          *other.bundle, other.model_map)),
      RuntimeProgramBindingResolutionError);
}

TEST(ProgramBindingCompilerTest,
     ProjectsCurrentOwnerFactsIntoExactArmAndGripperRuntimeParticipants) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path(), true);
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle, true), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto binding = compile_program_binding(
      *contract, valid_assignment(true), *context.bundle, context.model_map,
      catalog_store);

  const auto arm_target = resolve_runtime_program_target(
      *binding, TargetRoleId("arm"), CapabilityId("state.observed.current"),
      *context.bundle, context.model_map);
  const auto gripper_target = resolve_runtime_program_target(
      *binding, TargetRoleId("gripper"), CapabilityId("tool.gripper.command"),
      *context.bundle, context.model_map);
  const auto arm = build_runtime_participant_binding_dto(
      arm_target, active_facts_for(arm_target));
  const auto gripper = build_runtime_participant_binding_dto(
      gripper_target, active_facts_for(gripper_target));

  EXPECT_EQ(arm.controllable_component_id, gripper.controllable_component_id);
  EXPECT_NE(arm.command_target_id, gripper.command_target_id);
  ASSERT_TRUE(arm.control_path.has_value());
  ASSERT_TRUE(gripper.control_path.has_value());
  EXPECT_NE(arm.control_path->control_binding_id,
            gripper.control_path->control_binding_id);
  EXPECT_NE(arm.control_path->control_domain_id,
            gripper.control_path->control_domain_id);
  EXPECT_EQ(arm.resources.size(), 6U);
  std::vector<std::string> arm_source_joint_names;
  arm_source_joint_names.reserve(arm.resources.size());
  for (const auto &resource : arm.resources) {
    ASSERT_TRUE(resource.source_joint_name.has_value());
    arm_source_joint_names.push_back(*resource.source_joint_name);
    EXPECT_EQ(resource.resource_role_id.value(), "joints");
  }
  EXPECT_EQ(arm_source_joint_names,
            (std::vector<std::string>{"joint1", "joint2", "joint3", "joint4",
                                      "joint5", "joint6"}));
  EXPECT_TRUE(std::all_of(arm.resources.begin(), arm.resources.end(),
                          [](const auto &resource) {
                            return resource.coordinate_kind ==
                                   manyforge::core::CoordinateKind::kAngular;
                          }));
  ASSERT_EQ(gripper.resources.size(), 1U);
  ASSERT_TRUE(gripper.resources.front().source_joint_name.has_value());
  EXPECT_EQ(*gripper.resources.front().source_joint_name, "right_finger_joint");
  EXPECT_EQ(gripper.resources.front().coordinate_kind,
            manyforge::core::CoordinateKind::kLinear);
  ASSERT_TRUE(gripper.command_schema.has_value());
  EXPECT_EQ(gripper.command_schema->command_schema_id.value(),
            manyforge::core::kGripperPositionCommandSchemaId);
  EXPECT_EQ(gripper.control_path->active_control_configuration_id.value(),
            "active.native");
  EXPECT_TRUE(
      gripper.control_path->active_control_configuration_generation.nonzero());
  EXPECT_FALSE(gripper.control_path->provider_runtime_epoch.empty());
  ASSERT_EQ(arm.contact_grasp_bindings.size(), 1U);
  EXPECT_EQ(arm.contact_grasp_bindings, gripper.contact_grasp_bindings);
  EXPECT_EQ(arm.contact_grasp_bindings.front().runtime_object_id.value(),
            "selected_object");
}

TEST(ProgramBindingCompilerTest,
     RejectsMissingRuntimeEpochAndNonDesiredI6Route) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_context(root.path(), true);
  const auto contract = verify_program_binding_contract(
      make_program_bytes(*context.bundle, true), *context.bundle);
  FilesystemCatalogArtifactStore catalog_store(root.path());
  const auto binding = compile_program_binding(
      *contract, valid_assignment(true), *context.bundle, context.model_map,
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

} // namespace
