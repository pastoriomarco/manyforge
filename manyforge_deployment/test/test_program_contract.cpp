#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "content_identity_internal.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#include "scoped_procfd_root.hpp"

namespace {

using manyforge::core::CapabilityId;
using manyforge::core::ComponentRoleId;
using manyforge::core::FrameRoleId;
using manyforge::core::ProgramCapabilityDisposition;
using manyforge::core::ProgramId;
using manyforge::core::ProgramRevision;
using manyforge::core::ResourceRoleId;
using manyforge::core::TargetRoleId;
using manyforge::deployment::canonical_json;
using manyforge::deployment::CanonicalValue;
using manyforge::deployment::FilesystemProgramRepository;
using manyforge::deployment::FilesystemVerifiedProgramSource;
using manyforge::deployment::FilesystemVerifiedRetainedBundleSource;
using manyforge::deployment::ProgramContractError;
using manyforge::deployment::RetainedBundle;
using manyforge::deployment::verify_program_binding_contract;
using manyforge::deployment::detail::compute_content_identity;
using manyforge::deployment::detail::ContentIdentitySpec;
using manyforge::deployment::test::materialize_lite6_retained_consumer_fixture;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

class ScopedTrustedRoot final {
public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-program-contract-" +
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

struct ProgramOptions final {
  std::string catalog_content_version;
  std::vector<std::string> frame_roles{"base", "tcp"};
  std::vector<std::string> resource_roles{"joints"};
  std::vector<std::string> capability_ids{"state.observed.current"};
  bool include_legacy_robot{false};
  bool duplicate_tree_instance{false};
  bool include_contact_grasp{false};
  std::string value_source_kind;
  bool duplicate_value_source_parameter{false};
  bool include_node_policy{false};
  bool explicit_default_node_policy{false};
  bool include_allowed_skill{false};
};

[[nodiscard]] CanonicalValue
text_array(const std::vector<std::string> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue make_contact_grasp_policy() {
  CanonicalValue policy(CanonicalValue::Object{
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
                              CanonicalValue("arm_collision")},
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
  const std::string revision = compute_content_identity(
      policy, ContentIdentitySpec{
                  .identity_domain = "catalog-record-revision/contact_policy",
                  .api_version = "manyforge.contact_policy.v1",
                  .own_field = "version",
              });
  auto members = policy.as_object();
  for (auto &[name, value] : members) {
    if (name == "version") {
      value = CanonicalValue(revision);
      return CanonicalValue(std::move(members));
    }
  }
  throw std::logic_error("test contact policy omitted version");
}

[[nodiscard]] CanonicalValue make_program_root(const ProgramOptions &options) {
  CanonicalValue::Array capabilities;
  capabilities.reserve(options.capability_ids.size());
  for (const auto &capability_id : options.capability_ids) {
    capabilities.emplace_back(CanonicalValue::Object{
        {"capability_id", CanonicalValue(capability_id)},
        {"parameters", CanonicalValue(CanonicalValue::Object{})},
        {"disposition", CanonicalValue("required")},
    });
  }

  CanonicalValue::Array children;
  if (options.duplicate_tree_instance) {
    children.emplace_back(CanonicalValue::Object{
        {"node_id", CanonicalValue("sequence")},
        {"instance_id", CanonicalValue("root")},
        {"parameters", CanonicalValue(CanonicalValue::Object{})},
        {"children", CanonicalValue(CanonicalValue::Array{})},
    });
  }

  CanonicalValue::Object tree_parameters;
  CanonicalValue::Object tree{
      {"node_id", CanonicalValue("sequence")},
      {"instance_id", CanonicalValue("root")},
      {"parameters", CanonicalValue(tree_parameters)},
      {"children", CanonicalValue(std::move(children))},
  };
  if (!options.value_source_kind.empty()) {
    if (options.duplicate_value_source_parameter) {
      tree_parameters.emplace_back("velocity_scale", CanonicalValue(0.5));
      for (auto &[name, value] : tree) {
        if (name == "parameters") {
          value = CanonicalValue(tree_parameters);
          break;
        }
      }
    }
    CanonicalValue::Object source{
        {"kind", CanonicalValue(options.value_source_kind)}};
    if (options.value_source_kind == "program_parameter") {
      source.emplace_back("reference", CanonicalValue("velocity_scale"));
    } else if (options.value_source_kind == "blackboard_key") {
      source.emplace_back("reference", CanonicalValue("cycle.velocity_scale"));
    } else if (options.value_source_kind == "blackboard_field") {
      source.emplace_back("reference", CanonicalValue("perception.grasp_plan"));
      source.emplace_back("field", CanonicalValue("approach_pose"));
    }
    tree.emplace_back(
        "value_sources",
        CanonicalValue(CanonicalValue::Object{
            {"velocity_scale", CanonicalValue(std::move(source))}}));
  }
  if (options.include_node_policy || options.explicit_default_node_policy) {
    const bool defaults = options.explicit_default_node_policy;
    CanonicalValue::Array actions;
    CanonicalValue::Array skills;
    if (!defaults) {
      actions.emplace_back(options.include_allowed_skill ? "intervention"
                                                         : "runtime_override");
    }
    if (options.include_allowed_skill) {
      skills.emplace_back("retained-skill");
    }
    tree.emplace_back(
        "modification_policy",
        CanonicalValue(CanonicalValue::Object{
            {"runtime_override",
             CanonicalValue(defaults ? "forbidden" : "human_approval")},
            {"persistent_edit",
             CanonicalValue(defaults ? "forbidden" : "llm_allowed")},
            {"intervention_delegate",
             CanonicalValue(defaults ? "forbidden" : "human_only")},
        }));
    tree.emplace_back(
        "recovery_policy",
        CanonicalValue(CanonicalValue::Object{
            {"allow_intervention",
             CanonicalValue(options.include_allowed_skill)},
            {"allowed_recovery_actions", CanonicalValue(std::move(actions))},
            {"max_retries", CanonicalValue(defaults ? 0.0 : 2.0)},
            {"allowed_skills", CanonicalValue(std::move(skills))},
            {"persistence_ceiling", CanonicalValue("session_only")},
        }));
    tree.emplace_back("operator_stop_policy",
                      CanonicalValue(defaults ? "inherit" : "hold"));
  }

  CanonicalValue::Object root{
      {"api_version", CanonicalValue("manyforge.program.v2")},
      {"program_id", CanonicalValue("i6_contract_program")},
      {"program_revision", CanonicalValue(std::string(kZeroDigest))},
      {"program_profile", CanonicalValue("composable-cell-v1")},
      {"name", CanonicalValue("I6 Program contract fixture")},
      {"catalog",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.node_catalog.v2")},
           {"content_version", CanonicalValue(options.catalog_content_version)},
       })},
      {"component_roles",
       CanonicalValue(CanonicalValue::Object{
           {"robot",
            CanonicalValue(CanonicalValue::Object{
                {"required_component_type_tags",
                 CanonicalValue(
                     CanonicalValue::Array{CanonicalValue("articulated-arm")})},
                {"required_model_roles", CanonicalValue(CanonicalValue::Array{
                                             CanonicalValue("arm_model")})},
            })},
       })},
      {"target_roles",
       CanonicalValue(CanonicalValue::Object{
           {"arm",
            CanonicalValue(CanonicalValue::Object{
                {"component_role", CanonicalValue("robot")},
                {"required_capabilities",
                 CanonicalValue(std::move(capabilities))},
                {"required_frame_roles", text_array(options.frame_roles)},
                {"required_resource_roles", text_array(options.resource_roles)},
                {"required_collision_body_roles",
                 CanonicalValue(
                     CanonicalValue::Array{CanonicalValue("arm_collision")})},
                {"required_model_roles", CanonicalValue(CanonicalValue::Array{
                                             CanonicalValue("arm_model")})},
            })},
       })},
      {"tree", CanonicalValue(std::move(tree))},
  };
  if (options.include_legacy_robot) {
    root.emplace_back("robot", CanonicalValue(CanonicalValue::Object{
                                   {"robot_id", CanonicalValue("legacy_robot")},
                                   {"group_id", CanonicalValue("legacy_group")},
                               }));
  }
  if (options.include_contact_grasp) {
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
    root.emplace_back("contact_policies", CanonicalValue(CanonicalValue::Array{
                                              make_contact_grasp_policy()}));
  }
  return CanonicalValue(std::move(root));
}

[[nodiscard]] CanonicalValue with_program_revision(CanonicalValue root) {
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
      return CanonicalValue(std::move(members));
    }
  }
  throw std::logic_error("test Program omitted program_revision");
}

[[nodiscard]] std::shared_ptr<const RetainedBundle>
make_bundle(const std::filesystem::path &root) {
  const auto materialized = materialize_lite6_retained_consumer_fixture(root);
  const FilesystemVerifiedRetainedBundleSource source(root);
  auto bundle = source.get_verified(materialized.bundle->retained_bundle_id());
  if (bundle == nullptr) {
    throw std::logic_error("test could not reload retained bundle");
  }
  return bundle;
}

[[nodiscard]] ProgramOptions valid_options(const RetainedBundle &bundle) {
  ProgramOptions options;
  options.catalog_content_version =
      bundle.value().catalogs.node_environment.content_version;
  return options;
}

[[nodiscard]] std::shared_ptr<const RetainedBundle> maintained_motion_bundle() {
  const std::filesystem::path path(MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT);
  const manyforge::deployment::FilesystemScenarioCatalogRepository catalog(
      path, "maintained");
  const auto source = catalog.get_verified();
  if (source == nullptr)
    throw std::logic_error("maintained capture package was not prepared");
  for (const auto &row : source->rows()) {
    if (row.scenario_id == "lite6_pick_and_place") {
      return FilesystemVerifiedRetainedBundleSource(path).get_verified(
          row.retained_bundle_id);
    }
  }
  throw std::logic_error("maintained capture fixture has no Lite6 scenario");
}

void erase_member(CanonicalValue::Object &object, std::string_view name) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    throw std::logic_error("test could not find requested object member");
  }
  object.erase(found);
}

void replace_member(CanonicalValue::Object &object, std::string_view name,
                    CanonicalValue value) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    throw std::logic_error("test could not find requested object member");
  }
  found->second = std::move(value);
}

template <typename Mutation>
void mutate_object_member(CanonicalValue::Object &object, std::string_view name,
                          Mutation mutation) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    throw std::logic_error("test could not find requested object member");
  }
  auto child = found->second.as_object();
  mutation(child);
  found->second = CanonicalValue(std::move(child));
}

template <typename Mutation>
[[nodiscard]] CanonicalValue make_policy_program(const RetainedBundle &bundle,
                                                 Mutation mutation) {
  ProgramOptions options;
  options.catalog_content_version =
      bundle.value().catalogs.node_environment.content_version;
  options.include_node_policy = true;
  auto root = make_program_root(options);
  auto members = root.as_object();
  for (auto &[name, value] : members) {
    if (name != "tree") {
      continue;
    }
    auto tree = value.as_object();
    mutation(tree);
    value = CanonicalValue(std::move(tree));
    break;
  }
  return with_program_revision(CanonicalValue(std::move(members)));
}

TEST(ProgramContractTest,
     SealsExactCanonicalProgramAndRetainedCatalogContract) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const CanonicalValue program =
      with_program_revision(make_program_root(valid_options(*bundle)));
  const std::string bytes = canonical_json(program);

  const auto contract = verify_program_binding_contract(bytes, *bundle);

  ASSERT_NE(contract, nullptr);
  EXPECT_EQ(contract->canonical_program_bytes(), bytes);
  EXPECT_EQ(contract->normalized_program_root(), program);
  EXPECT_EQ(contract->value().program_id, ProgramId("i6_contract_program"));
  EXPECT_EQ(contract->value().node_environment,
            bundle->value().catalogs.node_environment);
  ASSERT_EQ(contract->value().component_roles.size(), 1U);
  EXPECT_EQ(contract->value().component_roles.front().component_role_id,
            ComponentRoleId("robot"));
  ASSERT_EQ(contract->value().target_roles.size(), 1U);
  const auto &target = contract->value().target_roles.front();
  EXPECT_EQ(target.target_role_id, TargetRoleId("arm"));
  EXPECT_EQ(target.component_role_id, ComponentRoleId("robot"));
  EXPECT_EQ(
      target.required_frame_roles,
      (std::vector<FrameRoleId>{FrameRoleId("base"), FrameRoleId("tcp")}));
  EXPECT_EQ(target.required_resource_roles,
            (std::vector<ResourceRoleId>{ResourceRoleId("joints")}));
  ASSERT_EQ(target.required_capabilities.size(), 1U);
  EXPECT_EQ(target.required_capabilities.front().capability_id,
            CapabilityId("state.observed.current"));
  EXPECT_EQ(target.required_capabilities.front().disposition,
            ProgramCapabilityDisposition::kRequired);
}

TEST(ProgramContractTest,
     DurableRepositoryPublishesAndReopensOneExactProgramRevision) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const std::string bytes = canonical_json(
      with_program_revision(make_program_root(valid_options(*bundle))));
  const auto contract = verify_program_binding_contract(bytes, *bundle);

  {
    FilesystemProgramRepository repository(root.path());
    EXPECT_TRUE(repository.put_if_absent(contract, *bundle));
    EXPECT_FALSE(repository.put_if_absent(contract, *bundle));
    EXPECT_EQ(repository.size(), 1U);
    EXPECT_EQ(repository.retained_bytes(), bytes.size());
  }

  FilesystemProgramRepository reopened(root.path());
  const auto read =
      reopened.get_verified(contract->value().program_id,
                            contract->value().program_revision, *bundle);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->value(), contract->value());
  EXPECT_EQ(read->canonical_program_bytes(), bytes);
  EXPECT_EQ(reopened.get_verified(contract->value().program_id,
                                  ProgramRevision(std::string(kZeroDigest)),
                                  *bundle),
            nullptr);
}

TEST(ProgramContractTest,
     VerifiedSourceReadsThroughProcfdAndLeavesMissingNamespaceAbsent) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const std::string bytes = canonical_json(
      with_program_revision(make_program_root(valid_options(*bundle))));
  const auto contract = verify_program_binding_contract(bytes, *bundle);
  {
    FilesystemProgramRepository writer(root.path());
    ASSERT_TRUE(writer.put_if_absent(contract, *bundle));
  }

  const auto entry_count_before = static_cast<std::size_t>(
      std::distance(std::filesystem::recursive_directory_iterator(root.path()),
                    std::filesystem::recursive_directory_iterator()));
  manyforge::deployment::test::ScopedProcfdRoot procfd(root.path());
  const FilesystemVerifiedProgramSource source(procfd.path());
  manyforge::deployment::test::ScopedPathReplacement replacement(root.path());
  const auto read =
      source.get_verified(contract->value().program_id,
                          contract->value().program_revision, *bundle);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->canonical_program_bytes(), bytes);
  EXPECT_EQ(source.get_verified(contract->value().program_id,
                                ProgramRevision(std::string(kZeroDigest)),
                                *bundle),
            nullptr);
  EXPECT_EQ(static_cast<std::size_t>(std::distance(
                std::filesystem::recursive_directory_iterator(root.path()),
                std::filesystem::recursive_directory_iterator())),
            0U);
  EXPECT_EQ(
      static_cast<std::size_t>(std::distance(
          std::filesystem::recursive_directory_iterator(replacement.backing()),
          std::filesystem::recursive_directory_iterator())),
      entry_count_before);

  ScopedTrustedRoot empty_root;
  manyforge::deployment::test::ScopedProcfdRoot empty_procfd(empty_root.path());
  const FilesystemVerifiedProgramSource empty_source(empty_procfd.path());
  EXPECT_EQ(empty_source.get_verified(contract->value().program_id,
                                      contract->value().program_revision,
                                      *bundle),
            nullptr);
  EXPECT_FALSE(std::filesystem::exists(empty_root.path() / "programs"));

  ScopedTrustedRoot unsafe_root;
  std::ofstream(unsafe_root.path() / "programs") << "not-a-directory";
  manyforge::deployment::test::ScopedProcfdRoot unsafe_procfd(
      unsafe_root.path());
  const FilesystemVerifiedProgramSource unsafe_source(unsafe_procfd.path());
  EXPECT_THROW(static_cast<void>(unsafe_source.get_verified(
                   contract->value().program_id,
                   contract->value().program_revision, *bundle)),
               manyforge::deployment::RetainedBundleError);
}

TEST(ProgramContractTest,
     ExtractsRevisionVerifiedPortableContactGraspTemplate) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  ProgramOptions options = valid_options(*bundle);
  options.include_contact_grasp = true;
  const auto contract = verify_program_binding_contract(
      canonical_json(with_program_revision(make_program_root(options))),
      *bundle);

  ASSERT_EQ(contract->value().contact_grasp_templates.size(), 1U);
  const auto &policy = contract->value().contact_grasp_templates.front();
  EXPECT_EQ(policy.contact_policy_id.value(), "selected_object_grasp");
  EXPECT_EQ(policy.runtime_object_id.value(), "selected_object");
  ASSERT_EQ(policy.allowed_body_selectors.size(), 1U);
  EXPECT_EQ(policy.allowed_body_selectors.front().component_role_id.value(),
            "robot");
  EXPECT_EQ(
      policy.allowed_body_selectors.front().collision_body_role_id.value(),
      "arm_collision");
  EXPECT_DOUBLE_EQ(policy.max_penetration_m, 0.002);
  EXPECT_DOUBLE_EQ(policy.terminal_window_start_fraction, 0.8);
}

TEST(ProgramContractTest, ExtractsSparseCompleteNodePolicies) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  ProgramOptions options = valid_options(*bundle);
  options.include_node_policy = true;
  const auto contract = verify_program_binding_contract(
      canonical_json(with_program_revision(make_program_root(options))),
      *bundle);

  ASSERT_EQ(contract->value().node_policies.size(), 1U);
  const auto &policy = contract->value().node_policies.front();
  EXPECT_EQ(policy.instance_id.value(), "root");
  EXPECT_EQ(policy.modification_policy.runtime_override,
            manyforge::core::ProgramModificationPolicyValue::kHumanApproval);
  EXPECT_EQ(policy.modification_policy.persistent_edit,
            manyforge::core::ProgramModificationPolicyValue::kLlmAllowed);
  EXPECT_EQ(policy.recovery_policy.allowed_recovery_actions,
            (std::vector<manyforge::core::ProgramRecoveryAction>{
                manyforge::core::ProgramRecoveryAction::kRuntimeOverride}));
  EXPECT_EQ(policy.operator_stop_policy,
            manyforge::core::ProgramOperatorStopPolicy::kHold);
}

TEST(ProgramContractTest,
     ExplicitCompleteDefaultNodePoliciesNormalizeToOmission) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto omitted = verify_program_binding_contract(
      canonical_json(
          with_program_revision(make_program_root(valid_options(*bundle)))),
      *bundle);
  ProgramOptions defaults = valid_options(*bundle);
  defaults.explicit_default_node_policy = true;
  auto explicit_root = with_program_revision(make_program_root(defaults));
  auto explicit_members = explicit_root.as_object();
  for (auto &[name, value] : explicit_members) {
    if (name == "program_revision") {
      value = CanonicalValue(omitted->value().program_revision.value());
    }
  }
  const auto explicit_defaults = verify_program_binding_contract(
      canonical_json(CanonicalValue(std::move(explicit_members))), *bundle);

  EXPECT_EQ(explicit_defaults->canonical_program_bytes(),
            omitted->canonical_program_bytes());
  EXPECT_EQ(explicit_defaults->value().program_revision,
            omitted->value().program_revision);
  EXPECT_TRUE(explicit_defaults->value().node_policies.empty());
}

TEST(ProgramContractTest, RejectsSkillIdsWithoutQualifiedRetainedReferents) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  ProgramOptions options = valid_options(*bundle);
  options.include_node_policy = true;
  options.include_allowed_skill = true;

  try {
    static_cast<void>(verify_program_binding_contract(
        canonical_json(with_program_revision(make_program_root(options))),
        *bundle));
    FAIL() << "nonempty allowed_skills unexpectedly verified";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(std::string(error.what())
                  .find("exact revision-qualified retained skill referent"),
              std::string::npos);
  }
}

TEST(ProgramContractTest,
     RawNodePolicyEnvelopeRejectsMalformedAndUnqualifiedRows) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto expect_rejected = [&](auto mutation) {
    EXPECT_THROW(
        static_cast<void>(verify_program_binding_contract(
            canonical_json(make_policy_program(*bundle, mutation)), *bundle)),
        ProgramContractError);
  };

  expect_rejected([](auto &tree) { erase_member(tree, "instance_id"); });
  expect_rejected([](auto &tree) {
    replace_member(tree, "modification_policy", CanonicalValue(nullptr));
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "modification_policy", [](auto &policy) {
      erase_member(policy, "persistent_edit");
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "modification_policy", [](auto &policy) {
      policy.emplace_back("unexpected", CanonicalValue(true));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "modification_policy", [](auto &policy) {
      replace_member(policy, "runtime_override",
                     CanonicalValue("operator_allowed"));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "modification_policy", [](auto &policy) {
      replace_member(policy, "intervention_delegate",
                     CanonicalValue("operator"));
    });
  });

  expect_rejected([](auto &tree) {
    replace_member(tree, "recovery_policy", CanonicalValue(nullptr));
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      erase_member(policy, "max_retries");
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      policy.emplace_back("unexpected", CanonicalValue(true));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "allow_intervention", CanonicalValue(0.0));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Object{}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("intervention"),
                         CanonicalValue("runtime_override"),
                         CanonicalValue("runtime_override")}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(
          policy, "allowed_recovery_actions",
          CanonicalValue(CanonicalValue::Array{CanonicalValue("unknown")}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("runtime_override"),
                         CanonicalValue("runtime_override")}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("runtime_override"),
                         CanonicalValue("intervention")}));
      replace_member(policy, "allow_intervention", CanonicalValue(true));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "allowed_recovery_actions",
                     CanonicalValue("intervention"));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "max_retries", CanonicalValue(true));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "max_retries", CanonicalValue(256.0));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(
          policy, "allowed_skills",
          CanonicalValue(CanonicalValue::Array{CanonicalValue("Upper")}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(
          policy, "allowed_skills",
          CanonicalValue(CanonicalValue::Array{CanonicalValue("z-skill"),
                                               CanonicalValue("a-skill")}));
      replace_member(policy, "allow_intervention", CanonicalValue(true));
      replace_member(policy, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("intervention")}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(
          policy, "allowed_skills",
          CanonicalValue(CanonicalValue::Array{CanonicalValue("a-skill"),
                                               CanonicalValue("a-skill")}));
      replace_member(policy, "allow_intervention", CanonicalValue(true));
      replace_member(policy, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("intervention")}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "allowed_skills",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue(std::string(129, 'a'))}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      CanonicalValue::Array skills;
      for (std::size_t index = 0U; index < 257U; ++index) {
        skills.emplace_back("skill-" + std::to_string(index));
      }
      replace_member(policy, "allowed_skills",
                     CanonicalValue(std::move(skills)));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "persistence_ceiling",
                     CanonicalValue("persistent"));
    });
  });
  expect_rejected([](auto &tree) {
    replace_member(tree, "operator_stop_policy", CanonicalValue("pause"));
  });
  expect_rejected([](auto &tree) {
    replace_member(tree, "instance_id", CanonicalValue("Root"));
  });
  expect_rejected([](auto &tree) {
    replace_member(tree, "instance_id", CanonicalValue(std::string(129, 'a')));
  });

  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "allow_intervention", CanonicalValue(true));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(
          policy, "allowed_skills",
          CanonicalValue(CanonicalValue::Array{CanonicalValue("a-skill")}));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "modification_policy", [](auto &policy) {
      replace_member(policy, "runtime_override", CanonicalValue("forbidden"));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "modification_policy", [](auto &modification) {
      replace_member(modification, "runtime_override",
                     CanonicalValue("forbidden"));
    });
    mutate_object_member(tree, "recovery_policy", [](auto &recovery) {
      replace_member(recovery, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("intervention")}));
      replace_member(recovery, "allow_intervention", CanonicalValue(true));
      replace_member(recovery, "persistence_ceiling",
                     CanonicalValue("runtime_override"));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "modification_policy", [](auto &modification) {
      replace_member(modification, "persistent_edit",
                     CanonicalValue("forbidden"));
    });
    mutate_object_member(tree, "recovery_policy", [](auto &recovery) {
      replace_member(recovery, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("intervention")}));
      replace_member(recovery, "allow_intervention", CanonicalValue(true));
      replace_member(recovery, "persistence_ceiling",
                     CanonicalValue("draft_patch"));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &recovery) {
      replace_member(recovery, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{}));
      replace_member(recovery, "max_retries", CanonicalValue(1.0));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &recovery) {
      replace_member(recovery, "allowed_recovery_actions",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("intervention")}));
      replace_member(recovery, "allow_intervention", CanonicalValue(true));
      replace_member(
          recovery, "allowed_skills",
          CanonicalValue(CanonicalValue::Array{CanonicalValue("a-skill")}));
    });
  });
}

TEST(ProgramContractTest, RejectsLegacySingletonRobotSurface) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  ProgramOptions options = valid_options(*bundle);
  options.include_legacy_robot = true;
  const std::string bytes =
      canonical_json(with_program_revision(make_program_root(options)));

  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(bytes, *bundle)),
      ProgramContractError);
}

TEST(ProgramContractTest, RejectsStaleCompleteProgramRevision) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  CanonicalValue::Object members =
      with_program_revision(make_program_root(valid_options(*bundle)))
          .as_object();
  for (auto &[name, value] : members) {
    if (name == "name") {
      value = CanonicalValue("Changed without a new revision");
    }
  }

  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(CanonicalValue(std::move(members))), *bundle)),
      ProgramContractError);
}

TEST(ProgramContractTest, RejectsUnsortedAndDuplicateRoleRequirementSets) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());

  ProgramOptions unsorted = valid_options(*bundle);
  unsorted.frame_roles = {"tcp", "base"};
  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(with_program_revision(make_program_root(unsorted))),
          *bundle)),
      ProgramContractError);

  ProgramOptions duplicate = valid_options(*bundle);
  duplicate.resource_roles = {"joints", "joints"};
  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(with_program_revision(make_program_root(duplicate))),
          *bundle)),
      ProgramContractError);
}

TEST(ProgramContractTest, RejectsUnknownCapabilityAndWrongNodeCatalog) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());

  ProgramOptions unknown_capability = valid_options(*bundle);
  unknown_capability.capability_ids = {"unknown.capability"};
  EXPECT_THROW(static_cast<void>(verify_program_binding_contract(
                   canonical_json(with_program_revision(
                       make_program_root(unknown_capability))),
                   *bundle)),
               ProgramContractError);

  ProgramOptions wrong_catalog = valid_options(*bundle);
  wrong_catalog.catalog_content_version += "-different";
  EXPECT_THROW(static_cast<void>(verify_program_binding_contract(
                   canonical_json(
                       with_program_revision(make_program_root(wrong_catalog))),
                   *bundle)),
               ProgramContractError);
}

TEST(ProgramContractTest, RejectsDuplicateTreeInstanceIdentity) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  ProgramOptions options = valid_options(*bundle);
  options.duplicate_tree_instance = true;

  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(with_program_revision(make_program_root(options))),
          *bundle)),
      ProgramContractError);
}

TEST(ProgramContractTest, SealsClosedProgramValueSourceForms) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  for (const std::string kind :
       {"program_parameter", "blackboard_key", "blackboard_field"}) {
    ProgramOptions options = valid_options(*bundle);
    options.value_source_kind = kind;
    EXPECT_NO_THROW(static_cast<void>(verify_program_binding_contract(
        canonical_json(with_program_revision(make_program_root(options))),
        *bundle)));
  }
}

TEST(ProgramContractTest, RejectsMalformedOrMultiplySuppliedValueSource) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());

  ProgramOptions unknown = valid_options(*bundle);
  unknown.value_source_kind = "environment_variable";
  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(with_program_revision(make_program_root(unknown))),
          *bundle)),
      ProgramContractError);

  ProgramOptions duplicate = valid_options(*bundle);
  duplicate.value_source_kind = "program_parameter";
  duplicate.duplicate_value_source_parameter = true;
  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(with_program_revision(make_program_root(duplicate))),
          *bundle)),
      ProgramContractError);
}

[[nodiscard]] CanonicalValue
selection_node(std::string instance, CanonicalValue groups,
               CanonicalValue::Array children = {}) {
  CanonicalValue::Object result{
      {"node_id", CanonicalValue(children.empty() ? "timer" : "sequence")},
      {"instance_id", CanonicalValue(std::move(instance))},
      {"parameters",
       CanonicalValue(
           children.empty()
               ? CanonicalValue::Object{{"duration_s", CanonicalValue(0.1)}}
               : CanonicalValue::Object{})},
      {"children", CanonicalValue(std::move(children))},
  };
  if (!groups.is_string() || groups.as_string() != "inherit") {
    result.emplace_back("execution_groups", std::move(groups));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue with_tree(const ProgramOptions &options,
                                       CanonicalValue tree) {
  auto members = make_program_root(options).as_object();
  replace_member(members, "tree", std::move(tree));
  return with_program_revision(CanonicalValue(std::move(members)));
}

[[nodiscard]] CanonicalValue synchronized_motion_node(std::string instance,
                                                      std::string group) {
  return CanonicalValue(CanonicalValue::Object{
      {"node_id", CanonicalValue("motion.move_to_joint_target")},
      {"instance_id", CanonicalValue(std::move(instance))},
      {"parameters",
       CanonicalValue(CanonicalValue::Object{
           {"target_role", CanonicalValue("arm")},
           {"base_frame_role", CanonicalValue("base")},
           {"tcp_frame_role", CanonicalValue("tcp")},
           {"goal", CanonicalValue(CanonicalValue::Array{
                        CanonicalValue(CanonicalValue::Object{
                            {"joint_name", CanonicalValue("joint1")},
                            {"position_rad", CanonicalValue(0.0)}})})}})},
      {"execution_groups", text_array({group})},
      {"children", CanonicalValue(CanonicalValue::Array{})},
  });
}

[[nodiscard]] CanonicalValue execution_group_definitions(
    std::initializer_list<std::pair<std::string, std::string>> definitions) {
  CanonicalValue::Object result;
  for (const auto &[name, policy] : definitions) {
    result.emplace_back(name,
                        CanonicalValue(CanonicalValue::Object{
                            {"execution_policy", CanonicalValue(policy)}}));
  }
  return CanonicalValue(std::move(result));
}

TEST(ProgramContractTest,
     CapturesOverlappingInheritedAndNeverStartedNodeSelections) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  // Default `inherit` is canonically omitted before calculating the revision.
  auto tree = selection_node(
      "root", CanonicalValue("all"),
      {
          selection_node(
              "a", text_array({"robot_a"}),
              {
                  selection_node("arm_a", CanonicalValue("inherit")),
                  selection_node("gripper_a",
                                 text_array({"all_grippers", "robot_a"})),
              }),
          selection_node(
              "b", text_array({"robot_b"}),
              {
                  selection_node("arm_b", CanonicalValue("inherit")),
                  selection_node("gripper_b",
                                 text_array({"all_grippers", "robot_b"})),
                  selection_node("logical_all", CanonicalValue("all")),
              }),
      });
  auto program_root = with_tree(valid_options(*bundle), tree).as_object();
  program_root.emplace_back(
      "execution_groups",
      execution_group_definitions({{"all_grippers", "independent"},
                                   {"robot_a", "independent"},
                                   {"robot_b", "independent"}}));
  const auto program =
      with_program_revision(CanonicalValue(std::move(program_root)));
  const auto contract =
      verify_program_binding_contract(canonical_json(program), *bundle);
  const auto &table = contract->execution_table();
  ASSERT_EQ(table.nodes.size(), 8U);
  ASSERT_EQ(table.named_selections.size(), 3U);
  EXPECT_TRUE(contract->value().node_policies.empty());
  EXPECT_EQ(table.named_selections[0].group_id, "all_grippers");
  EXPECT_EQ(table.named_selections[0].node_indices,
            (std::vector<std::uint32_t>{4U, 5U}));
  EXPECT_EQ(table.named_selections[1].node_indices,
            (std::vector<std::uint32_t>{0U, 1U, 4U}));
  EXPECT_EQ(table.named_selections[2].node_indices,
            (std::vector<std::uint32_t>{2U, 3U, 5U}));
  for (const auto &node : table.nodes)
    EXPECT_TRUE(node.target_effects.empty());
}

TEST(ProgramContractTest, CapturesOnlyExplicitParallelCoordinatedMotionSets) {
  const auto bundle = maintained_motion_bundle();
  auto options = valid_options(*bundle);
  options.capability_ids = {"execution.feedback.phase_events",
                            "execution.trajectory.execute",
                            "planning.motion.joint_goal",
                            "planning.profile.balanced",
                            "planning.trajectory.time_parameterized",
                            "state.observed.current",
                            "validation.trajectory.non_collision"};
  CanonicalValue parallel(CanonicalValue::Object{
      {"node_id", CanonicalValue("parallel")},
      {"instance_id", CanonicalValue("paired_motion")},
      {"parameters", CanonicalValue(CanonicalValue::Object{
                         {"success_policy", CanonicalValue("all")}})},
      {"children", CanonicalValue(CanonicalValue::Array{
                       synchronized_motion_node("arm_a", "cell_pair"),
                       synchronized_motion_node("arm_b", "cell_pair")})},
  });
  auto root = with_tree(options, std::move(parallel)).as_object();
  root.emplace_back("execution_groups", execution_group_definitions(
                                            {{"cell_pair", "common_start"}}));
  const auto contract = verify_program_binding_contract(
      canonical_json(with_program_revision(CanonicalValue(std::move(root)))),
      *bundle);

  ASSERT_EQ(contract->execution_table().named_selections.size(), 1U);
  const auto &selection = contract->execution_table().named_selections.front();
  EXPECT_EQ(selection.group_id, "cell_pair");
  EXPECT_EQ(selection.execution_policy,
            manyforge::core::ProgramExecutionPolicy::kCommonStart);
  ASSERT_EQ(selection.node_indices.size(), 2U);
  for (const auto index : selection.node_indices) {
    const auto &node = contract->execution_table().nodes[index];
    EXPECT_EQ(node.owner_api, "kernel.operation");
    EXPECT_TRUE(std::any_of(node.target_effects.begin(),
                            node.target_effects.end(), [](const auto &effect) {
                              return effect.effectful &&
                                     effect.capability_id ==
                                         CapabilityId(
                                             "execution.trajectory.execute");
                            }));
  }
}

TEST(ProgramContractTest,
     SelectionDefaultsAndSetOrderNormalizeBeforeIdentityCheck) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto options = valid_options(*bundle);
  auto program = with_program_revision(make_program_root(options));
  auto source = program.as_object();
  mutate_object_member(source, "tree", [](auto &tree) {
    tree.emplace_back("execution_groups", CanonicalValue("inherit"));
  });
  const auto contract = verify_program_binding_contract(
      canonical_json(CanonicalValue(source)), *bundle);
  EXPECT_EQ(contract->normalized_program_root(), program);
  EXPECT_EQ(contract->execution_table().nodes.size(), 1U);
  EXPECT_TRUE(contract->execution_table().named_selections.empty());
  auto grouped_program =
      with_tree(options, selection_node("root", text_array({"a", "z"})))
          .as_object();
  grouped_program.emplace_back(
      "execution_groups",
      execution_group_definitions(
          {{"a", "independent"}, {"z", "independent"}}));
  program = with_program_revision(CanonicalValue(std::move(grouped_program)));
  source = program.as_object();
  mutate_object_member(source, "tree", [](auto &tree) {
    replace_member(tree, "execution_groups", text_array({"z", "a"}));
  });
  EXPECT_EQ(verify_program_binding_contract(
                canonical_json(CanonicalValue(source)), *bundle)
                ->normalized_program_root(),
            program);
}

TEST(ProgramContractTest,
     RejectsMalformedGroupSetsAndBoundedInheritanceExpansion) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto options = valid_options(*bundle);
  for (const auto &invalid : CanonicalValue::Array{
           CanonicalValue(nullptr), CanonicalValue(true),
           CanonicalValue("robot_a"), text_array({}), text_array({"all"}),
           text_array({"inherit"}), text_array({"a", "a"}),
           text_array({"UPPER"}), text_array({std::string(129U, 'a')})}) {
    EXPECT_THROW(
        static_cast<void>(verify_program_binding_contract(
            canonical_json(with_tree(options, selection_node("root", invalid))),
            *bundle)),
        ProgramContractError);
  }
  std::vector<std::string> groups;
  for (std::size_t index = 0U;
       index < manyforge::core::kMaxProgramExecutionGroups; ++index) {
    groups.push_back(std::string("g").append(std::to_string(index)));
  }
  std::sort(groups.begin(), groups.end());
  CanonicalValue::Array children;
  for (std::size_t index = 0U; index < 25U; ++index) {
    children.push_back(selection_node("child" + std::to_string(index),
                                      CanonicalValue("inherit")));
  }
  const auto expanded = with_tree(
      options, selection_node("root", text_array(groups), std::move(children)));
  try {
    static_cast<void>(
        verify_program_binding_contract(canonical_json(expanded), *bundle));
    FAIL() << "unbounded inherited membership accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(std::string(error.what()).find("memberships exceed fixed bound"),
              std::string::npos);
  }
  const auto too_many_names = with_tree(
      options, selection_node("root", text_array(groups),
                              {selection_node("additional",
                                              text_array({"another_group"}))}));
  try {
    static_cast<void>(verify_program_binding_contract(
        canonical_json(too_many_names), *bundle));
    FAIL() << "excess group names accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(std::string(error.what()).find("group names exceed fixed bound"),
              std::string::npos);
  }
}

TEST(ProgramContractTest, CapturesOnlyCatalogQualifiedNodeTargetAssociations) {
  const auto bundle = maintained_motion_bundle();
  auto options = valid_options(*bundle);
  options.capability_ids = {"execution.stop.soft", "state.observed.current"};
  auto tree = selection_node("stop_arm", text_array({"robot_a"})).as_object();
  replace_member(tree, "node_id", CanonicalValue("kernel.stop_operation"));
  replace_member(tree, "parameters",
                 CanonicalValue(CanonicalValue::Object{
                     {"target_role", CanonicalValue("arm")}}));
  const auto program_with_robot_a_group =
      [&options](CanonicalValue grouped_tree) {
        auto grouped_program =
            with_tree(options, std::move(grouped_tree)).as_object();
        grouped_program.emplace_back(
            "execution_groups",
            execution_group_definitions({{"robot_a", "independent"}}));
        return with_program_revision(
            CanonicalValue(std::move(grouped_program)));
      };
  const auto verified = verify_program_binding_contract(
      canonical_json(program_with_robot_a_group(CanonicalValue(tree))),
      *bundle);
  const auto &node = verified->execution_table().nodes.front();
  EXPECT_EQ(node.owner_api, "kernel.stop");
  ASSERT_EQ(node.target_effects.size(), 1U);
  EXPECT_EQ(node.target_effects.front().target_role_id, TargetRoleId("arm"));
  EXPECT_EQ(node.target_effects.front().capability_id,
            CapabilityId("execution.stop.soft"));
  EXPECT_TRUE(node.target_effects.front().effectful);
  // Declaring a capability somewhere in the Program does not grant it to
  // every node; an unknown literal role or absent node requirement rejects.
  replace_member(tree, "parameters",
                 CanonicalValue(CanonicalValue::Object{
                     {"target_role", CanonicalValue("unrelated")}}));
  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(program_with_robot_a_group(CanonicalValue(tree))),
          *bundle)),
      ProgramContractError);
  replace_member(tree, "parameters",
                 CanonicalValue(CanonicalValue::Object{
                     {"target_role", CanonicalValue("arm")}}));
  options.capability_ids = {"state.observed.current"};
  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(program_with_robot_a_group(CanonicalValue(tree))),
          *bundle)),
      ProgramContractError);
  replace_member(tree, "node_id", CanonicalValue("not_in_catalog"));
  EXPECT_THROW(
      static_cast<void>(verify_program_binding_contract(
          canonical_json(program_with_robot_a_group(CanonicalValue(tree))),
          *bundle)),
      ProgramContractError);
}

TEST(ProgramContractTest,
     CapturesEffectiveConditionalCapabilitiesWithoutDynamicScope) {
  const auto bundle = maintained_motion_bundle();
  auto options = valid_options(*bundle);
  options.capability_ids = {"execution.feedback.phase_events",
                            "execution.trajectory.execute",
                            "planning.motion.joint_goal",
                            "planning.profile.balanced",
                            "planning.profile.first_feasible",
                            "planning.trajectory.time_parameterized",
                            "state.observed.current",
                            "validation.trajectory.non_collision"};
  auto tree = selection_node("move", CanonicalValue("all")).as_object();
  replace_member(tree, "node_id",
                 CanonicalValue("motion.move_to_joint_target"));
  replace_member(
      tree, "parameters",
      CanonicalValue(CanonicalValue::Object{
          {"target_role", CanonicalValue("arm")},
          {"base_frame_role", CanonicalValue("base")},
          {"tcp_frame_role", CanonicalValue("tcp")},
          {"goal", CanonicalValue(CanonicalValue::Array{
                       CanonicalValue(CanonicalValue::Object{
                           {"joint_name", CanonicalValue("joint1")},
                           {"position_rad", CanonicalValue(0.0)}})})},
      }));
  const auto verify = [&] {
    return verify_program_binding_contract(
        canonical_json(with_tree(options, CanonicalValue(tree))), *bundle);
  };
  auto contract = verify();
  const auto has = [](const auto &table, const char *capability) {
    const auto &effects = table.nodes.front().target_effects;
    return std::any_of(effects.begin(), effects.end(), [&](const auto &effect) {
      return effect.capability_id == CapabilityId(capability);
    });
  };
  const auto effectful = [](const auto &table, const char *capability) {
    const auto &effects = table.nodes.front().target_effects;
    const auto found =
        std::find_if(effects.begin(), effects.end(), [&](const auto &effect) {
          return effect.capability_id == CapabilityId(capability);
        });
    return found != effects.end() && found->effectful;
  };
  EXPECT_TRUE(has(contract->execution_table(), "planning.profile.balanced"));
  EXPECT_TRUE(
      effectful(contract->execution_table(), "execution.trajectory.execute"));
  EXPECT_FALSE(
      effectful(contract->execution_table(), "state.observed.current"));
  EXPECT_FALSE(
      has(contract->execution_table(), "planning.profile.first_feasible"));
  mutate_object_member(tree, "parameters", [](auto &parameters) {
    parameters.emplace_back("planning_profile",
                            CanonicalValue("first_feasible"));
  });
  contract = verify();
  EXPECT_FALSE(has(contract->execution_table(), "planning.profile.balanced"));
  EXPECT_TRUE(
      has(contract->execution_table(), "planning.profile.first_feasible"));
  for (const auto &invalid :
       CanonicalValue::Array{CanonicalValue("bogus"), CanonicalValue(nullptr),
                             CanonicalValue(true), text_array({"balanced"})}) {
    mutate_object_member(tree, "parameters", [&](auto &parameters) {
      replace_member(parameters, "planning_profile", invalid);
    });
    try {
      static_cast<void>(verify());
      FAIL() << "invalid condition stamped an incomplete effect capture";
    } catch (const ProgramContractError &error) {
      EXPECT_NE(
          std::string(error.what()).find("invalid catalog condition value"),
          std::string::npos);
    }
  }
  mutate_object_member(tree, "parameters", [](auto &parameters) {
    replace_member(parameters, "planning_profile", CanonicalValue("balanced"));
  });
  // Narrow native capture proof, not a runnable Program: keep only literal
  // target/default-condition fields consumed at this boundary. Full motion
  // parameters would hit the independent YAML-node bound first. Seven target
  // associations per node exercise the real 100,000 association limit.
  auto capture_only = tree;
  erase_member(capture_only, "execution_groups");
  replace_member(capture_only, "parameters",
                 CanonicalValue(CanonicalValue::Object{
                     {"target_role", CanonicalValue("arm")}}));
  CanonicalValue::Array branches;
  for (std::size_t branch = 0U; branch < 2U; ++branch) {
    CanonicalValue::Array motions;
    for (std::size_t index = 0U; index < 7200U; ++index) {
      auto motion = capture_only;
      replace_member(motion, "instance_id",
                     CanonicalValue(std::string("n")
                                        .append(std::to_string(branch))
                                        .append("_")
                                        .append(std::to_string(index))));
      motions.emplace_back(std::move(motion));
    }
    branches.push_back(
        selection_node(std::string("branch").append(std::to_string(branch)),
                       CanonicalValue("inherit"), std::move(motions)));
  }
  const auto many_effects =
      with_tree(options, selection_node("root", CanonicalValue("all"),
                                        std::move(branches)));
  try {
    static_cast<void>(
        verify_program_binding_contract(canonical_json(many_effects), *bundle));
    FAIL() << "excess target associations accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(
        std::string(error.what()).find("target effects exceed fixed bound"),
        std::string::npos);
  }
  mutate_object_member(tree, "parameters", [](auto &parameters) {
    erase_member(parameters, "target_role");
  });
  tree.emplace_back(
      "value_sources",
      CanonicalValue(CanonicalValue::Object{
          {"target_role", CanonicalValue(CanonicalValue::Object{
                              {"kind", CanonicalValue("program_parameter")},
                              {"reference", CanonicalValue("chosen_target")}})},
      }));
  try {
    static_cast<void>(verify());
    FAIL() << "dynamic target selected an execution scope";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(std::string(error.what())
                  .find("effect scope/condition requires a literal parameter"),
              std::string::npos);
  }
}

} // namespace
