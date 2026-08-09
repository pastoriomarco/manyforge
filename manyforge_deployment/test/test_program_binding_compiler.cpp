#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
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
using manyforge::deployment::compile_program_binding;
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
using manyforge::deployment::test::materialize_lite6_retained_consumer_fixture;
using manyforge::deployment::test::materialize_lite6_native_motion_retained_consumer_fixture;

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

[[nodiscard]] CanonicalValue make_program_root(const RetainedBundle &bundle) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.program.v1")},
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
      {"tree", CanonicalValue(CanonicalValue::Object{
                   {"node_id", CanonicalValue("sequence")},
                   {"instance_id", CanonicalValue("root")},
                   {"parameters", CanonicalValue(CanonicalValue::Object{})},
                   {"children", CanonicalValue(CanonicalValue::Array{})},
               })},
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
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           CanonicalValue::Object{
               {"zone_id", CanonicalValue("tool_contact")},
               {"body_pairs",
                CanonicalValue(CanonicalValue::Array{CanonicalValue(
                    CanonicalValue::Object{
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
          {"keys",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(
               CanonicalValue::Object{
                   {"key", CanonicalValue("selected_object")},
                   {"type", CanonicalValue("scene_resource")},
                   {"access", CanonicalValue("read_write")},
                   {"persistence", CanonicalValue("run")},
               })})},
      }));
  root.emplace_back(
      "contact_policies",
      CanonicalValue(CanonicalValue::Array{
          CanonicalValue(std::move(policy_members))}));
  return CanonicalValue(std::move(root));
}

[[nodiscard]] std::string make_program_bytes(const RetainedBundle &bundle,
                                             bool aggregate_gripper = false) {
  CanonicalValue root = aggregate_gripper ? make_aggregate_program_root(bundle)
                                          : make_program_root(bundle);
  const std::string revision =
      compute_content_identity(root, ContentIdentitySpec{
                                         .identity_domain = "program-revision",
                                         .api_version = "manyforge.program.v1",
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

  std::set<manyforge::core::ProviderInstanceId> providers;
  if (target.control_binding().has_value()) {
    providers.insert(target.control_binding()->provider_instance_id);
  }
  if (target.capability_provider_binding().has_value()) {
    providers.insert(
        target.capability_provider_binding()->provider_instance_id);
  }
  std::vector<RuntimeProviderEpochFactDTO> provider_epochs;
  for (const auto &id : providers) {
    provider_epochs.push_back(
        {id, manyforge::core::RuntimeEpoch("epoch." + id.value())});
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
     DerivesDeterministicDirectControlMotionBindingWithoutScenarioProgram) {
  ScopedTrustedRoot root;
  const BindingCompilerContext context = make_native_motion_context(root.path());
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
  EXPECT_EQ(first->value().component_role_bindings.front()
                .component_role_id.value(),
            manyforge::deployment::kDirectControlComponentRoleId);
  ASSERT_EQ(first->value().target_role_bindings.size(), 1U);
  const auto &target = first->value().target_role_bindings.front();
  EXPECT_EQ(target.target_role_id.value(),
            manyforge::deployment::kDirectControlTargetRoleId);
  EXPECT_EQ(target.command_target_id.value(), kLite6ArmTargetId);
  EXPECT_FALSE(target.frame_role_bindings.empty());
  EXPECT_NE(std::find_if(target.frame_role_bindings.begin(),
                         target.frame_role_bindings.end(),
                         [](const auto &frame) {
                           return frame.frame_role_id.value() ==
                                  manyforge::deployment::
                                      kDirectControlBaseFrameRoleId;
                         }),
            target.frame_role_bindings.end());
  EXPECT_NE(std::find_if(target.frame_role_bindings.begin(),
                         target.frame_role_bindings.end(),
                         [](const auto &frame) {
                           return frame.frame_role_id.value() ==
                                  manyforge::deployment::
                                      kDirectControlTcpFrameRoleId;
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
  const BindingCompilerContext context = make_native_motion_context(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());

  EXPECT_THROW(
      static_cast<void>(compile_direct_control_motion_program_binding(
          manyforge::core::ControllableComponentId("missing_component"),
          manyforge::core::CommandTargetId(std::string(kLite6ArmTargetId)),
          *context.bundle, context.model_map, catalog_store)),
      ProgramBindingCompilerError);
  EXPECT_THROW(
      static_cast<void>(compile_direct_control_motion_program_binding(
          manyforge::core::ControllableComponentId(
              std::string(kLite6ControllableComponentId)),
          manyforge::core::CommandTargetId("missing_target"), *context.bundle,
          context.model_map, catalog_store)),
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
  const auto& target = first->value().target_role_bindings.front();
  EXPECT_EQ(target.target_role_id.value(),
            manyforge::deployment::kDirectControlTargetRoleId);
  EXPECT_EQ(target.command_target_id.value(), kLite6GripperTargetId);
  ASSERT_EQ(target.capability_bindings.size(), 1U);
  const auto& capability = target.capability_bindings.front();
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
  EXPECT_EQ(arm.contact_grasp_bindings,
            gripper.contact_grasp_bindings);
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

  auto wrong_route = active_facts_for(target);
  wrong_route.control_route_id = manyforge::core::ControlRouteId("other");
  EXPECT_THROW(static_cast<void>(
                   build_runtime_participant_binding_dto(target, wrong_route)),
               RuntimeProgramBindingResolutionError);
}

} // namespace
