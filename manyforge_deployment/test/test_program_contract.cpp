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

#include <manyforge_core/active_tcp_blackboard_key.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/program_reference_validator.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "content_identity_internal.hpp"
#include "program_root_fixture.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#include "scoped_procfd_root.hpp"

using manyforge::deployment::test::ProgramOptions;
using manyforge::deployment::test::text_array;
using manyforge::deployment::test::make_program_root;
using manyforge::deployment::test::with_program_revision;

namespace {

using manyforge::core::CapabilityId;
using manyforge::core::FrameRoleId;
using manyforge::core::ProgramCapabilityDisposition;
using manyforge::core::ProgramId;
using manyforge::core::ProgramRevision;
using manyforge::deployment::canonical_json;
using manyforge::deployment::CanonicalValue;
using manyforge::deployment::FilesystemProgramRepository;
using manyforge::deployment::FilesystemVerifiedProgramSource;
using manyforge::deployment::FilesystemVerifiedRetainedBundleSource;
using manyforge::deployment::parse_strict_json_model_yaml;
using manyforge::deployment::ProgramContractError;
using manyforge::deployment::RetainedBundle;
using manyforge::deployment::verify_program_binding_contract;
using manyforge::deployment::detail::compute_content_identity;
using manyforge::deployment::detail::ContentIdentitySpec;
using manyforge::deployment::test::materialize_lite6_retained_consumer_fixture;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

TEST(ProgramContractFormatTest, RejectsV2BeforeNormalizationWithTypedVersions) {
  const CanonicalValue v2(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.program.v2")}});
  try {
    static_cast<void>(manyforge::deployment::normalize_program_authoring(v2));
    FAIL() << "v2 format was accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_EQ(error.code(), "program_format_unsupported");
    ASSERT_TRUE(error.unsupported_format().has_value());
    EXPECT_EQ(error.unsupported_format()->actual_api_version, "manyforge.program.v2");
    EXPECT_EQ(error.unsupported_format()->supported_api_version, "manyforge.program.v3");
  }
}

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
  const manyforge::core::CellId cell_id("lite6_pick_place_cell");
  const FilesystemVerifiedRetainedBundleSource source(path);
  std::shared_ptr<const RetainedBundle> selected;
  for (const auto &entry :
       std::filesystem::directory_iterator(path / "retained-bundles" / "sha256")) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }
    const auto bundle = source.get_verified(manyforge::core::RetainedBundleId(
        "sha256:" + entry.path().stem().string()));
    if (bundle != nullptr && bundle->value().cell_graph.cell_id == cell_id &&
        bundle->value().deployment_artifact_id.value() ==
            "lite6_lite_gripper_pick_and_place") {
      if (selected != nullptr) {
        throw std::logic_error("maintained capture package has duplicate Lite6 motion bundles");
      }
      selected = bundle;
    }
  }
  if (selected == nullptr) {
    throw std::logic_error(
        "maintained capture package has no exact verified Lite6 motion bundle");
  }
  return selected;
}

// A durable Program is verified again against the retained bundle and catalogs
// under its own trusted root, so a private root must hold the maintained
// package those Programs were sealed against.
void stage_maintained_package(const std::filesystem::path &root) {
  std::filesystem::copy(std::filesystem::path(MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT), root,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing);
}

// Use the same durable, exact candidate inputs as the production verifier.
[[nodiscard]] std::shared_ptr<const manyforge::deployment::ProgramBindingContract>
verify_fixture_program(std::string_view bytes, const RetainedBundle &bundle,
                       const std::filesystem::path &root) {
  const auto &products = bundle.value().compiled_products.symbol_role_maps;
  if (products.size() != 1U)
    throw std::logic_error("maintained Program fixture has no sole model map");
  const manyforge::deployment::FilesystemVerifiedRetainedModelMapSource maps(root);
  const auto map = maps.get_verified(manyforge::deployment::RetainedModelMapQuery{
      bundle.retained_bundle_id(), products.front().product_id});
  if (!map)
    throw std::logic_error("maintained Program fixture model map is unavailable");
  const manyforge::deployment::FilesystemCatalogArtifactStore catalog(root);
  return verify_program_binding_contract(bytes, bundle, map, catalog);
}

[[nodiscard]] std::shared_ptr<const manyforge::deployment::ProgramBindingContract>
verify_maintained_program(std::string_view bytes, const RetainedBundle &bundle) {
  return verify_fixture_program(bytes, bundle, MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT);
}

CanonicalValue direct_target(std::string component, std::string command) {
  return CanonicalValue(CanonicalValue::Object{{"component_id", CanonicalValue(std::move(component))},
                                                {"command_target_id", CanonicalValue(std::move(command))}});
}
CanonicalValue direct_frame(std::string component, std::string frame) {
  return CanonicalValue(CanonicalValue::Object{{"component_id", CanonicalValue(std::move(component))},
                                                {"frame_id", CanonicalValue(std::move(frame))}});
}
CanonicalValue arm_tcp_selector() {
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("entry")}, {"target", direct_target("lite6_robot", "arm")},
      {"frame", direct_frame("lite6_robot", "flange")}});
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

  const auto contract = verify_fixture_program(bytes, *bundle, root.path());

  ASSERT_NE(contract, nullptr);
  EXPECT_EQ(contract->canonical_program_bytes(), bytes);
  EXPECT_EQ(contract->normalized_program_root(), program);
  EXPECT_EQ(contract->value().program_id, ProgramId("contract_program"));
  EXPECT_EQ(contract->value().node_environment,
            bundle->value().catalogs.node_environment);
  EXPECT_TRUE(contract->value().target_requirements.empty());
  EXPECT_TRUE(contract->value().frame_requirements.empty());
  EXPECT_TRUE(contract->value().signal_requirements.empty());
}

TEST(ProgramContractTest, ContextFreeVerificationFailsClosedBeforeEffects) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto program = with_program_revision(make_program_root(valid_options(*bundle)));
  EXPECT_THROW(static_cast<void>(verify_program_binding_contract(canonical_json(program), *bundle)),
               manyforge::deployment::ProgramReferenceValidationIncomplete);
}

TEST(ProgramContractTest, SealsLogicOnlyProgramAndRefusesLegacyRoleMaps) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto logic = with_program_revision(make_program_root(valid_options(*bundle)));
  const auto contract = verify_fixture_program(canonical_json(logic), *bundle, root.path());
  ASSERT_NE(contract, nullptr);
  EXPECT_TRUE(contract->value().target_requirements.empty());
  EXPECT_EQ(contract->normalized_program_root(), logic);
  for (const auto *legacy : {"component_roles", "target_roles"}) {
    auto members = logic.as_object();
    members.emplace_back(legacy, CanonicalValue(CanonicalValue::Object{}));
    EXPECT_THROW(static_cast<void>(verify_fixture_program(
        canonical_json(with_program_revision(CanonicalValue(std::move(members)))), *bundle, root.path())),
        ProgramContractError);
  }
}

TEST(ProgramContractTest,
     DurableRepositoryPublishesAndReopensOneExactProgramRevision) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const std::string bytes = canonical_json(
      with_program_revision(make_program_root(valid_options(*bundle))));
  const auto contract = verify_fixture_program(bytes, *bundle, root.path());

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
  const auto contract = verify_fixture_program(bytes, *bundle, root.path());
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
  const auto contract = verify_fixture_program(
      canonical_json(with_program_revision(make_program_root(options))),
      *bundle, root.path());

  ASSERT_EQ(contract->value().contact_grasp_templates.size(), 1U);
  const auto &policy = contract->value().contact_grasp_templates.front();
  EXPECT_FALSE(policy.bodies_from_motion_endpoint);
  EXPECT_EQ(policy.contact_policy_id.value(), "selected_object_grasp");
  EXPECT_EQ(policy.runtime_object_id.value(), "selected_object");
  ASSERT_EQ(policy.allowed_body_selectors.size(), 1U);
  EXPECT_EQ(policy.allowed_body_selectors.front().body.component_id.value(),
            "lite6_arm");
  EXPECT_EQ(
      policy.allowed_body_selectors.front().body.collision_body_id.value(),
      "arm_collision");
  const auto &envelope = std::get<manyforge::core::ContactGraspEnvelopeDTO>(policy.envelope);
  EXPECT_DOUBLE_EQ(envelope.max_penetration_m, 0.002);
  EXPECT_DOUBLE_EQ(envelope.terminal_window_start_fraction, 0.8);
}

TEST(ProgramContractTest, MotionEndpointContactSelectorIsExclusiveAndHasNoIds) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  auto options = valid_options(*bundle);
  options.include_contact_grasp = true;
  const auto document = [&](CanonicalValue::Array pairs) {
    auto policy =
        manyforge::deployment::test::make_contact_grasp_policy().as_object();
    for (auto &[name, value] : policy) {
      if (name != "zones")
        continue;
      auto zones = value.as_array();
      auto zone = zones.front().as_object();
      replace_member(zone, "body_pairs", CanonicalValue(std::move(pairs)));
      zones.front() = CanonicalValue(std::move(zone));
      value = CanonicalValue(std::move(zones));
    }
    const auto revision = compute_content_identity(
        CanonicalValue(policy),
        ContentIdentitySpec{.identity_domain =
                                "catalog-record-revision/contact_policy",
                            .api_version = "manyforge.contact_policy.v1",
                            .own_field = "version"});
    replace_member(policy, "version", CanonicalValue(revision));
    auto program = make_program_root(options).as_object();
    replace_member(program, "contact_policies",
                   CanonicalValue(CanonicalValue::Array{
                       CanonicalValue(std::move(policy))}));
    return canonical_json(
        with_program_revision(CanonicalValue(std::move(program))));
  };
  const auto pair = [](CanonicalValue::Object robot) {
    return CanonicalValue(CanonicalValue::Object{
        {"body_a", CanonicalValue(std::move(robot))},
        {"body_b",
         CanonicalValue(CanonicalValue::Object{
             {"kind", CanonicalValue("runtime_object")},
             {"runtime_object_id", CanonicalValue("selected_object")}})}});
  };
  const auto endpoint = pair({{"kind", CanonicalValue("motion_endpoint")}});
  const auto contract =
      verify_fixture_program(document({endpoint}), *bundle, root.path());
  ASSERT_EQ(contract->value().contact_grasp_templates.size(), 1U);
  EXPECT_TRUE(contract->value()
                  .contact_grasp_templates.front()
                  .bodies_from_motion_endpoint);
  EXPECT_TRUE(contract->value()
                  .contact_grasp_templates.front()
                  .allowed_body_selectors.empty());
  EXPECT_THROW(
      static_cast<void>(verify_fixture_program(
          document(
              {endpoint, pair({{"kind", CanonicalValue("component_model_body")},
                               {"body", CanonicalValue(CanonicalValue::Object{
                                   {"component_id", CanonicalValue("lite6_arm")},
                                   {"collision_body_id", CanonicalValue("arm_collision")}})}})}),
          *bundle, root.path())),
      ProgramContractError);
  EXPECT_THROW(
      static_cast<void>(verify_fixture_program(
          document({pair({{"kind", CanonicalValue("motion_endpoint")},
                          {"body", CanonicalValue(CanonicalValue::Object{
                              {"component_id", CanonicalValue("lite6_arm")},
                              {"collision_body_id", CanonicalValue("arm_collision")}})}})}),
          *bundle, root.path())),
      ProgramContractError);
}

TEST(ProgramContractTest, ExtractsSparseCompleteNodePolicies) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  ProgramOptions options = valid_options(*bundle);
  options.include_node_policy = true;
  const auto contract = verify_fixture_program(
      canonical_json(with_program_revision(make_program_root(options))),
      *bundle, root.path());

  ASSERT_EQ(contract->value().node_policies.size(), 1U);
  const auto &policy = contract->value().node_policies.front();
  EXPECT_EQ(policy.instance_id.value(), "root");
  EXPECT_TRUE(policy.recovery_policy.allow_intervention);
  EXPECT_EQ(policy.recovery_policy.max_retries, 2U);
  EXPECT_EQ(policy.operator_stop_policy,
            manyforge::core::ProgramOperatorStopPolicy::kHold);
}

TEST(ProgramContractTest,
     ExplicitCompleteDefaultNodePoliciesNormalizeToOmission) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto omitted = verify_fixture_program(
      canonical_json(
          with_program_revision(make_program_root(valid_options(*bundle)))),
      *bundle, root.path());
  ProgramOptions defaults = valid_options(*bundle);
  defaults.explicit_default_node_policy = true;
  auto explicit_root = with_program_revision(make_program_root(defaults));
  auto explicit_members = explicit_root.as_object();
  for (auto &[name, value] : explicit_members) {
    if (name == "program_revision") {
      value = CanonicalValue(omitted->value().program_revision.value());
    }
  }
  const auto explicit_defaults = verify_fixture_program(
      canonical_json(CanonicalValue(std::move(explicit_members))), *bundle, root.path());

  EXPECT_EQ(explicit_defaults->canonical_program_bytes(),
            omitted->canonical_program_bytes());
  EXPECT_EQ(explicit_defaults->value().program_revision,
            omitted->value().program_revision);
  EXPECT_TRUE(explicit_defaults->value().node_policies.empty());
}

TEST(ProgramContractTest, ZeroRetryBudgetIsUnlimitedNotDefault) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto contract = verify_fixture_program(
      canonical_json(make_policy_program(*bundle, [](auto &tree) {
        mutate_object_member(tree, "recovery_policy", [](auto &policy) {
          replace_member(policy, "max_retries", CanonicalValue(0.0));
        });
      })),
      *bundle, root.path());

  ASSERT_EQ(contract->value().node_policies.size(), 1U);
  const auto &policy = contract->value().node_policies.front();
  EXPECT_TRUE(policy.recovery_policy.allow_intervention);
  EXPECT_EQ(policy.recovery_policy.max_retries, 0U);
}

TEST(ProgramContractTest, InterventionStopScopeDefaultsAndNormalizesAll) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto omitted = verify_fixture_program(
      canonical_json(make_policy_program(*bundle, [](auto &) {})), *bundle, root.path());
  auto explicit_members =
      make_policy_program(*bundle, [](auto &tree) {
        mutate_object_member(tree, "recovery_policy", [](auto &policy) {
          policy.emplace_back("intervention_stop_scope", CanonicalValue("all"));
        });
      }).as_object();
  replace_member(explicit_members, "program_revision",
                 CanonicalValue(omitted->value().program_revision.value()));
  const auto explicit_all = verify_fixture_program(
      canonical_json(CanonicalValue(std::move(explicit_members))), *bundle, root.path());
  EXPECT_EQ(omitted->value()
                .node_policies.front()
                .recovery_policy.intervention_stop_scope,
            manyforge::core::ProgramInterventionStopScope::kAll);
  EXPECT_EQ(omitted->canonical_program_bytes(),
            explicit_all->canonical_program_bytes());
  const auto own_groups = verify_fixture_program(
      canonical_json(make_policy_program(
          *bundle,
          [](auto &tree) {
            mutate_object_member(tree, "recovery_policy", [](auto &policy) {
              policy.emplace_back("intervention_stop_scope",
                                  CanonicalValue("own_groups"));
            });
          })),
      *bundle, root.path());
  EXPECT_EQ(own_groups->value()
                .node_policies.front()
                .recovery_policy.intervention_stop_scope,
            manyforge::core::ProgramInterventionStopScope::kOwnGroups);
  EXPECT_NE(own_groups->value().program_revision,
            omitted->value().program_revision);
  for (const auto &invalid : {CanonicalValue("own_group"), CanonicalValue(true),
                              CanonicalValue(nullptr)}) {
    EXPECT_THROW(static_cast<void>(verify_fixture_program(
                     canonical_json(make_policy_program(
                         *bundle,
                         [&](auto &tree) {
                           mutate_object_member(
                               tree, "recovery_policy", [&](auto &policy) {
                                 policy.emplace_back("intervention_stop_scope",
                                                     invalid);
                               });
                         })),
                     *bundle, root.path())),
                 ProgramContractError);
  }
}

TEST(ProgramContractTest,
     RawNodePolicyEnvelopeRejectsMalformedAndUnqualifiedRows) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto expect_rejected = [&](auto mutation) {
    EXPECT_THROW(
        static_cast<void>(verify_fixture_program(
            canonical_json(make_policy_program(*bundle, mutation)), *bundle, root.path())),
        ProgramContractError);
  };

  expect_rejected([](auto &tree) { erase_member(tree, "instance_id"); });
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
      erase_member(policy, "allow_intervention");
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
      replace_member(policy, "max_retries", CanonicalValue(-1.0));
    });
  });
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      replace_member(policy, "max_retries", CanonicalValue(1.5));
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

  // Removed policy forms are refused, never reinterpreted.
  expect_rejected([](auto &tree) {
    tree.emplace_back(
        "modification_policy",
        CanonicalValue(CanonicalValue::Object{
            {"intervention_delegate", CanonicalValue("forbidden")},
            {"persistent_edit", CanonicalValue("forbidden")},
            {"runtime_override", CanonicalValue("forbidden")},
        }));
  });
  for (const char *removed : {"allowed_recovery_actions", "allowed_skills"}) {
    expect_rejected([removed](auto &tree) {
      mutate_object_member(tree, "recovery_policy", [removed](auto &policy) {
        policy.emplace_back(removed, CanonicalValue(CanonicalValue::Array{}));
      });
    });
  }
  expect_rejected([](auto &tree) {
    mutate_object_member(tree, "recovery_policy", [](auto &policy) {
      policy.emplace_back("persistence_ceiling", CanonicalValue("session_only"));
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
      static_cast<void>(verify_fixture_program(bytes, *bundle, root.path())),
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
      static_cast<void>(verify_fixture_program(
          canonical_json(CanonicalValue(std::move(members))), *bundle, root.path())),
      ProgramContractError);
}

TEST(ProgramContractTest, RejectsLegacyRoleRequirementsAndWrongNodeCatalog) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  auto legacy = make_program_root(valid_options(*bundle)).as_object();
  legacy.emplace_back("target_roles", CanonicalValue(CanonicalValue::Object{
      {"arm", CanonicalValue(CanonicalValue::Object{{"required_resource_roles", text_array({"joints", "joints"})}})}}));
  EXPECT_THROW(static_cast<void>(verify_fixture_program(
      canonical_json(with_program_revision(CanonicalValue(std::move(legacy)))), *bundle, root.path())), ProgramContractError);
  auto wrong_catalog = valid_options(*bundle);
  wrong_catalog.catalog_content_version += "-different";
  EXPECT_THROW(static_cast<void>(verify_fixture_program(
      canonical_json(with_program_revision(make_program_root(wrong_catalog))), *bundle, root.path())), ProgramContractError);
}

TEST(ProgramContractTest, RejectsDuplicateTreeInstanceIdentity) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  ProgramOptions options = valid_options(*bundle);
  options.duplicate_tree_instance = true;

  EXPECT_THROW(
      static_cast<void>(verify_fixture_program(
          canonical_json(with_program_revision(make_program_root(options))),
          *bundle, root.path())),
      ProgramContractError);
}

TEST(ProgramContractTest, SealsClosedProgramValueSourceForms) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  for (const std::string kind :
       {"program_parameter", "blackboard_key", "blackboard_field"}) {
    ProgramOptions options = valid_options(*bundle);
    options.value_source_kind = kind;
    EXPECT_NO_THROW(static_cast<void>(verify_fixture_program(
        canonical_json(with_program_revision(make_program_root(options))),
        *bundle, root.path())));
  }
}

TEST(ProgramContractTest, RejectsRemovedParameterRuntimeMutability) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  ProgramOptions options = valid_options(*bundle);
  options.value_source_kind = "program_parameter";
  auto members = make_program_root(options).as_object();
  for (auto &[name, value] : members) {
    if (name != "parameters") continue;
    auto rows = value.as_array();
    auto row = rows.front().as_object();
    row.emplace_back("runtime_mutability", CanonicalValue("before_run"));
    rows.front() = CanonicalValue(std::move(row));
    value = CanonicalValue(std::move(rows));
  }
  try {
    static_cast<void>(verify_fixture_program(
        canonical_json(with_program_revision(CanonicalValue(std::move(members)))),
        *bundle, root.path()));
    FAIL() << "a removed runtime_mutability field was accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(std::string(error.what()).find("runtime_mutability was removed"),
              std::string::npos);
  }
}

TEST(ProgramContractTest, RejectsMalformedValueSourceAndKeepsLiterals) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());

  ProgramOptions unknown = valid_options(*bundle);
  unknown.value_source_kind = "environment_variable";
  EXPECT_THROW(
      static_cast<void>(verify_fixture_program(
          canonical_json(with_program_revision(make_program_root(unknown))),
          *bundle, root.path())),
      ProgramContractError);

  // A literal stays beside its whole-value source: it is what "Type values"
  // restores when the source is removed.
  ProgramOptions kept_literal = valid_options(*bundle);
  kept_literal.value_source_kind = "program_parameter";
  kept_literal.duplicate_value_source_parameter = true;
  EXPECT_NO_THROW(static_cast<void>(verify_fixture_program(
      canonical_json(with_program_revision(make_program_root(kept_literal))),
      *bundle, root.path())));
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
           {"target", direct_target("lite6_robot", "arm")},
           {"base_frame", direct_frame("lite6_robot", "base")},
           {"tcp_endpoint", arm_tcp_selector()},
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

[[nodiscard]] CanonicalValue
automatic_retry_program(const RetainedBundle &bundle,
                        const std::string &node_id,
                        CanonicalValue::Object extra_parameters = {},
                        CanonicalValue::Object value_sources = {}) {
  auto options = valid_options(bundle);
  CanonicalValue::Object parameters{
      {"target", direct_target("lite6_robot", "arm")},
      {"base_frame", direct_frame("lite6_robot", "base")},
      {"tcp_endpoint", arm_tcp_selector()},
  };
  if (node_id == "motion.move_to_joint_target") {
    parameters.emplace_back(
        "goal",
        CanonicalValue(CanonicalValue::Array{CanonicalValue(
            CanonicalValue::Object{{"joint_name", CanonicalValue("joint1")},
                                   {"position_rad", CanonicalValue(0.0)}})}));
  } else {
    CanonicalValue pose(CanonicalValue::Object{
        {"position_m",
         CanonicalValue(CanonicalValue::Array{
             CanonicalValue(0.0), CanonicalValue(0.0), CanonicalValue(0.0)})},
        {"orientation",
         CanonicalValue(CanonicalValue::Object{
             {"xyzw", CanonicalValue(CanonicalValue::Array{
                          CanonicalValue(0.0), CanonicalValue(0.0),
                          CanonicalValue(0.0), CanonicalValue(1.0)})}})},
    });
    parameters.emplace_back(node_id == "motion.move_to_pose" ? "goal" : "path",
                            node_id == "motion.move_to_pose"
                                ? pose
                                : CanonicalValue(CanonicalValue::Array{pose, pose}));
  }
  parameters.insert(parameters.end(), extra_parameters.begin(),
                    extra_parameters.end());
  CanonicalValue::Object node{
      {"node_id", CanonicalValue(node_id)},
      {"instance_id", CanonicalValue("move")},
      {"parameters", CanonicalValue(std::move(parameters))},
      {"children", CanonicalValue(CanonicalValue::Array{})},
  };
  if (!value_sources.empty())
    node.emplace_back("value_sources",
                      CanonicalValue(std::move(value_sources)));
  return with_tree(options, CanonicalValue(std::move(node)));
}

TEST(ProgramContractTest,
     AcceptsLiteralAutomaticRetryPolicyWithoutKernelBudgetCapture) {
  const auto bundle = maintained_motion_bundle();
  for (const std::string node_id :
       {"motion.move_to_joint_target", "motion.move_to_pose",
        "motion.follow_cartesian_path"}) {
    SCOPED_TRACE(node_id);
    const auto defaults = verify_maintained_program(
        canonical_json(automatic_retry_program(*bundle, node_id)), *bundle);
    ASSERT_EQ(defaults->execution_table().nodes.size(), 1U);
    for (const double count : {0.0, 4.0, 10.0}) {
      for (const auto *mode : {"absolute", "relative"}) {
        const auto authored = verify_maintained_program(
            canonical_json(automatic_retry_program(
                *bundle, node_id,
                {{"automatic_retries", CanonicalValue(count)},
                 {"target_mode", CanonicalValue(mode)}})),
            *bundle);
        // Retry policy remains in the Program revision, not Kernel capture.
        EXPECT_EQ(authored->execution_table(), defaults->execution_table());
      }
    }
  }
  const auto timer = verify_maintained_program(
      canonical_json(with_tree(valid_options(*bundle),
                               selection_node("timer", CanonicalValue("all")))),
      *bundle);
  ASSERT_EQ(timer->execution_table().nodes.size(), 1U);
}

TEST(ProgramContractTest, SealsLiteralMotionPriorityFromQualifiedCatalog) {
  ScopedTrustedRoot root;
  stage_maintained_package(root.path());
  const auto bundle = maintained_motion_bundle();
  for (const std::string node_id :
       {"motion.move_to_joint_target", "motion.move_to_pose",
        "motion.follow_cartesian_path"}) {
    SCOPED_TRACE(node_id);
    const auto omitted = verify_maintained_program(
        canonical_json(automatic_retry_program(*bundle, node_id)), *bundle);
    EXPECT_EQ(omitted->execution_table().nodes.front().preemption_priority, 0U);
    for (const double priority : {0.0, 1.0, 100.0}) {
      const auto authored = verify_maintained_program(
          canonical_json(automatic_retry_program(
              *bundle, node_id,
              {{"preemption_priority", CanonicalValue(priority)}})),
          *bundle);
      EXPECT_EQ(authored->execution_table().nodes.front().preemption_priority,
                static_cast<std::uint8_t>(priority));
    }
    for (const auto &invalid :
         {CanonicalValue(-1.0), CanonicalValue(100.5), CanonicalValue(101.0),
          CanonicalValue(256.0), CanonicalValue(true), CanonicalValue("1")}) {
      EXPECT_THROW(
          static_cast<void>(verify_maintained_program(
              canonical_json(automatic_retry_program(
                  *bundle, node_id, {{"preemption_priority", invalid}})),
              *bundle)),
          ProgramContractError);
    }
    EXPECT_THROW(static_cast<void>(verify_maintained_program(
                     canonical_json(automatic_retry_program(
                         *bundle, node_id, {},
                         {{"preemption_priority",
                           CanonicalValue(CanonicalValue::Object{
                               {"kind", CanonicalValue("expression")},
                               {"expression", CanonicalValue("1")}})}})),
                     *bundle)),
                 ProgramContractError);
  }
  const auto captured = verify_maintained_program(
      canonical_json(automatic_retry_program(
          *bundle, "motion.move_to_joint_target",
          {{"preemption_priority", CanonicalValue(100.0)}})),
      *bundle);
  FilesystemProgramRepository repository(root.path());
  ASSERT_TRUE(repository.put_if_absent(captured, *bundle));
  const auto reopened =
      repository.get_verified(captured->value().program_id,
                              captured->value().program_revision, *bundle);
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->execution_table(), captured->execution_table());
}

TEST(ProgramContractTest, RejectsMalformedAutomaticRetryCountBeforeNarrowing) {
  const auto bundle = maintained_motion_bundle();
  const std::vector<CanonicalValue> malformed{
      CanonicalValue(-1.0),  CanonicalValue(0.5),         CanonicalValue(11.0),
      CanonicalValue(256.0), CanonicalValue(1'000'000.0), CanonicalValue(true),
      CanonicalValue("3")};
  for (const std::string node_id :
       {"motion.move_to_joint_target", "motion.move_to_pose",
        "motion.follow_cartesian_path"}) {
    for (const auto &count : malformed) {
      SCOPED_TRACE(node_id + ": " + canonical_json(count));
      EXPECT_THROW(static_cast<void>(verify_maintained_program(
                       canonical_json(automatic_retry_program(
                           *bundle, node_id, {{"automatic_retries", count}})),
                       *bundle)),
                   ProgramContractError);
    }
  }
}

TEST(ProgramContractTest, AutomaticRetryPolicyRejectsValueSources) {
  const auto bundle = maintained_motion_bundle();
  for (const auto *parameter :
       {"automatic_retries", "automatic_retry_delay_s"}) {
    try {
      static_cast<void>(verify_maintained_program(
          canonical_json(automatic_retry_program(
              *bundle, "motion.move_to_joint_target", {},
              {{parameter, CanonicalValue(CanonicalValue::Object{
                               {"kind", CanonicalValue("expression")},
                               {"expression", CanonicalValue("3")}})}})),
          *bundle));
      FAIL() << "automatic retry policy accepted a dynamic source";
    } catch (const ProgramContractError &error) {
      EXPECT_NE(std::string(error.what()).find(parameter), std::string::npos);
      EXPECT_NE(
          std::string(error.what())
              .find("cannot dynamically select runtime/binding semantics"),
          std::string::npos);
    }
  }
}

TEST(ProgramContractTest, AutomaticRetryPolicyPreservesDynamicTargetMode) {
  const auto bundle = maintained_motion_bundle();
  auto program =
      automatic_retry_program(
          *bundle, "motion.move_to_joint_target",
          {{"automatic_retries", CanonicalValue(4.0)}},
          {{"target_mode", CanonicalValue(CanonicalValue::Object{
                               {"kind", CanonicalValue("program_parameter")},
                               {"reference", CanonicalValue("mode")}})}})
          .as_object();
  program.emplace_back(
      "parameters",
      CanonicalValue(CanonicalValue::Array{CanonicalValue(
          CanonicalValue::Object{{"parameter_id", CanonicalValue("mode")},
                                 {"type", CanonicalValue("string")},
                                 {"default", CanonicalValue("relative")}})}));
  const auto contract = verify_maintained_program(
      canonical_json(with_program_revision(CanonicalValue(std::move(program)))),
      *bundle);
  ASSERT_EQ(contract->execution_table().nodes.size(), 1U);
}

TEST(ProgramContractTest,
     AutomaticRetryPolicyRemainsInProgramRevisionAcrossRepositoryReopen) {
  ScopedTrustedRoot root;
  stage_maintained_package(root.path());
  const auto bundle = maintained_motion_bundle();
  const auto contract = verify_maintained_program(
      canonical_json(automatic_retry_program(
          *bundle, "motion.move_to_joint_target",
          {{"automatic_retries", CanonicalValue(4.0)}})),
      *bundle);
  const auto changed = verify_maintained_program(
      canonical_json(automatic_retry_program(
          *bundle, "motion.move_to_joint_target",
          {{"automatic_retries", CanonicalValue(6.0)}})),
      *bundle);
  EXPECT_NE(contract->value().program_revision,
            changed->value().program_revision);
  EXPECT_EQ(contract->execution_table(), changed->execution_table());
  EXPECT_NE(contract->canonical_program_bytes(), changed->canonical_program_bytes());
  {
    FilesystemProgramRepository repository(root.path());
    ASSERT_TRUE(repository.put_if_absent(contract, *bundle));
  }
  FilesystemProgramRepository reopened(root.path());
  const auto read =
      reopened.get_verified(contract->value().program_id,
                            contract->value().program_revision, *bundle);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->execution_table(), contract->execution_table());
  EXPECT_EQ(read->canonical_program_bytes(),
            contract->canonical_program_bytes());
}

[[nodiscard]] CanonicalValue
stop_policy_node(std::string instance, std::string node_id,
                 std::string policy = "inherit",
                 CanonicalValue::Array children = {}) {
  CanonicalValue::Object result{
      {"node_id", CanonicalValue(node_id)},
      {"instance_id", CanonicalValue(std::move(instance))},
      {"parameters",
       CanonicalValue(
           node_id == "timer"
               ? CanonicalValue::Object{{"duration_s", CanonicalValue(0.1)}}
               : CanonicalValue::Object{})},
      {"children", CanonicalValue(std::move(children))},
  };
  if (policy != "inherit")
    result.emplace_back("operator_stop_policy",
                        CanonicalValue(std::move(policy)));
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue
gripper_stop_policy_node(std::string instance, std::string policy = "continue",
                         const std::vector<std::string> &groups = {}) {
  auto node = stop_policy_node(std::move(instance), "gripper.command_position",
                               std::move(policy))
                  .as_object();
  replace_member(node, "parameters",
                 CanonicalValue(CanonicalValue::Object{
                     {"target", direct_target("lite_gripper", "gripper")},
                     {"position", CanonicalValue(CanonicalValue::Object{
                                      {"kind", CanonicalValue("linear_m")},
                                      {"value_m", CanonicalValue(0.01)}})},
                 }));
  if (!groups.empty())
    node.emplace_back("execution_groups", text_array(groups));
  return CanonicalValue(std::move(node));
}

[[nodiscard]] CanonicalValue gripper_stop_policy_program(
    const ProgramOptions &options, CanonicalValue tree,
    CanonicalValue groups = CanonicalValue(CanonicalValue::Object{})) {
  auto program = with_tree(options, std::move(tree)).as_object();
  if (!groups.as_object().empty())
    program.emplace_back("execution_groups", std::move(groups));
  return with_program_revision(CanonicalValue(std::move(program)));
}

[[nodiscard]] CanonicalValue
grouped_stop_policy_node(std::string instance, std::string node_id,
                         std::string policy, CanonicalValue::Array children,
                         const std::vector<std::string> &groups) {
  auto node = stop_policy_node(std::move(instance), std::move(node_id),
                               std::move(policy), std::move(children))
                  .as_object();
  node.emplace_back("execution_groups", text_array(groups));
  return CanonicalValue(std::move(node));
}

void expect_catalog_gripper_effects(
    const manyforge::core::ProgramExecutionNodeDTO &node) {
  EXPECT_EQ(node.owner_api, "kernel.command_target");
  ASSERT_EQ(node.target_effects.size(), 2U);
  EXPECT_EQ(node.target_effects[0].target, (manyforge::core::ProgramTargetReferenceDTO{manyforge::core::ControllableComponentId("lite_gripper"), manyforge::core::CommandTargetId("gripper")}));
  EXPECT_EQ(node.target_effects[0].capability_id,
            CapabilityId("tool.gripper.command"));
  EXPECT_TRUE(node.target_effects[0].effectful);
  EXPECT_EQ(node.target_effects[1].target, (manyforge::core::ProgramTargetReferenceDTO{manyforge::core::ControllableComponentId("lite_gripper"), manyforge::core::CommandTargetId("gripper")}));
  EXPECT_EQ(node.target_effects[1].capability_id,
            CapabilityId("tool.gripper.feedback"));
  EXPECT_FALSE(node.target_effects[1].effectful);
}

TEST(ProgramContractTest, CapturesBehaviorStopPolicyDefaultsAndCatalogKinds) {
  using Policy = manyforge::core::ProgramOperatorStopPolicy;
  const auto bundle = maintained_motion_bundle();
  const auto options = valid_options(*bundle);
  const auto contract = verify_maintained_program(
      canonical_json(gripper_stop_policy_program(
          options,
          grouped_stop_policy_node(
              "a_root", "sequence", "inherit",
              {grouped_stop_policy_node(
                  "b_composite", "sequence", "inherit",
                  {grouped_stop_policy_node(
                      "c_decorator", "inverter", "inherit",
                      {gripper_stop_policy_node(
                          "d_leaf", "inherit",
                          {"leaf_only", "mixed", "whole"})},
                      {"whole"})},
                  {"mixed", "whole"})},
              {"whole"}),
          execution_group_definitions({{"leaf_only", "independent"},
                                       {"mixed", "independent"},
                                       {"whole", "independent"}}))),
      *bundle);
  const auto &table = contract->execution_table();
  const auto &nodes = table.nodes;
  ASSERT_EQ(nodes.size(), 4U);
  EXPECT_TRUE(contract->value().node_policies.empty());
  for (std::size_t index = 0U; index < nodes.size(); ++index) {
    EXPECT_EQ(nodes[index].effective_operator_stop_policy,
              index == 3U ? Policy::kHold : Policy::kContinue);
    if (index == 0U)
      EXPECT_FALSE(nodes[index].parent_node_index.has_value());
    else
      EXPECT_EQ(nodes[index].parent_node_index, index - 1U);
  }
  expect_catalog_gripper_effects(nodes[3]);
  ASSERT_EQ(table.named_selections.size(), 3U);
  EXPECT_EQ(table.named_selections[0].node_indices,
            (std::vector<std::uint32_t>{3U}));
  EXPECT_EQ(table.named_selections[1].node_indices,
            (std::vector<std::uint32_t>{1U, 3U}));
  EXPECT_EQ(table.named_selections[2].node_indices,
            (std::vector<std::uint32_t>{0U, 1U, 2U, 3U}));
  // Root inheritance takes precedence for an actual catalog gripper leaf.
  const auto leaf_root = verify_maintained_program(
      canonical_json(gripper_stop_policy_program(
          options, gripper_stop_policy_node("root", "inherit"))),
      *bundle);
  ASSERT_EQ(leaf_root->execution_table().nodes.size(), 1U);
  const auto &root_node = leaf_root->execution_table().nodes.front();
  EXPECT_EQ(root_node.effective_operator_stop_policy, Policy::kContinue);
  EXPECT_FALSE(root_node.parent_node_index.has_value());
  expect_catalog_gripper_effects(root_node);
}

TEST(ProgramContractTest,
     CapturesLocalExplicitOverridesWithoutFoldingHoldAncestors) {
  using Policy = manyforge::core::ProgramOperatorStopPolicy;
  const auto bundle = maintained_motion_bundle();
  const auto contract = verify_maintained_program(
      canonical_json(gripper_stop_policy_program(
          valid_options(*bundle),
          grouped_stop_policy_node(
              "z_root", "sequence", "hold",
              {grouped_stop_policy_node(
                   "a_inherited_hold", "sequence", "inherit",
                   {gripper_stop_policy_node(
                       "b_explicit_continue", "continue",
                       {"branch_whole", "child_only", "mixed", "whole"})},
                   {"branch_whole", "whole"}),
               grouped_stop_policy_node(
                   "c_explicit_continue", "sequence", "continue",
                   {grouped_stop_policy_node(
                        "d_inherited_continue", "inverter", "inherit",
                        {gripper_stop_policy_node("e_default_leaf", "inherit",
                                                  {"whole"})},
                        {"whole"}),
                    gripper_stop_policy_node("f_explicit_hold", "hold",
                                             {"whole"})},
                   {"whole"})},
              {"mixed", "whole"}),
          execution_group_definitions({{"branch_whole", "independent"},
                                       {"child_only", "independent"},
                                       {"mixed", "independent"},
                                       {"whole", "independent"}}))),
      *bundle);
  const auto &table = contract->execution_table();
  const auto &nodes = table.nodes;
  ASSERT_EQ(nodes.size(), 7U);
  const std::vector<Policy> policies{
      Policy::kHold, Policy::kContinue, Policy::kContinue, Policy::kContinue,
      Policy::kHold, Policy::kHold,     Policy::kHold};
  const std::vector<std::uint32_t> parents{6U, 0U, 6U, 2U, 3U, 2U};
  for (std::size_t index = 0U; index < nodes.size(); ++index) {
    EXPECT_EQ(nodes[index].effective_operator_stop_policy, policies[index]);
    if (index < parents.size())
      EXPECT_EQ(nodes[index].parent_node_index, parents[index]);
    else
      EXPECT_FALSE(nodes[index].parent_node_index.has_value());
  }
  for (const auto index : {1U, 4U, 5U})
    expect_catalog_gripper_effects(nodes[index]);
  ASSERT_EQ(table.named_selections.size(), 4U);
  EXPECT_EQ(table.named_selections[0].node_indices,
            (std::vector<std::uint32_t>{0U, 1U}));
  EXPECT_EQ(table.named_selections[1].node_indices,
            (std::vector<std::uint32_t>{1U}));
  EXPECT_EQ(table.named_selections[2].node_indices,
            (std::vector<std::uint32_t>{1U, 6U}));
  EXPECT_EQ(table.named_selections[3].node_indices,
            (std::vector<std::uint32_t>{0U, 1U, 2U, 3U, 4U, 5U, 6U}));
  const manyforge::core::ProgramExecutionNodeDTO legacy_node{
      manyforge::core::ProgramNodeInstanceId("legacy"), "kernel.operation", {}};
  EXPECT_EQ(legacy_node.effective_operator_stop_policy, Policy::kHold);
  EXPECT_FALSE(legacy_node.parent_node_index.has_value());
}

TEST(ProgramContractTest,
     CapturesLocalPoliciesAndTopologyForPartialHeldSubtrees) {
  using Policy = manyforge::core::ProgramOperatorStopPolicy;
  const auto bundle = maintained_motion_bundle();
  auto child = gripper_stop_policy_node("a_child").as_object();
  child.emplace_back("execution_groups",
                     text_array({"child_only", "mixed", "whole"}));
  auto sibling = gripper_stop_policy_node("b_sibling").as_object();
  sibling.emplace_back("execution_groups", text_array({"whole"}));
  auto branch = stop_policy_node("c_branch", "sequence", "hold",
                                 {CanonicalValue(std::move(child)),
                                  CanonicalValue(std::move(sibling))})
                    .as_object();
  branch.emplace_back("execution_groups", text_array({"mixed", "whole"}));
  auto program =
      with_tree(valid_options(*bundle),
                stop_policy_node("z_root", "sequence", "inherit",
                                 {CanonicalValue(std::move(branch))}))
          .as_object();
  program.emplace_back("execution_groups", execution_group_definitions(
                                               {{"child_only", "independent"},
                                                {"mixed", "independent"},
                                                {"whole", "independent"}}));
  const auto contract = verify_maintained_program(
      canonical_json(with_program_revision(CanonicalValue(std::move(program)))),
      *bundle);
  const auto &table = contract->execution_table();
  ASSERT_EQ(table.nodes.size(), 4U);
  EXPECT_EQ(table.nodes[0].effective_operator_stop_policy, Policy::kContinue);
  EXPECT_EQ(table.nodes[1].effective_operator_stop_policy, Policy::kContinue);
  EXPECT_EQ(table.nodes[2].effective_operator_stop_policy, Policy::kHold);
  EXPECT_EQ(table.nodes[3].effective_operator_stop_policy, Policy::kContinue);
  for (const auto index : {0U, 1U}) {
    const auto &node = table.nodes[index];
    EXPECT_EQ(node.owner_api, "kernel.command_target");
    ASSERT_EQ(node.target_effects.size(), 2U);
    EXPECT_EQ(node.target_effects[0].target, (manyforge::core::ProgramTargetReferenceDTO{manyforge::core::ControllableComponentId("lite_gripper"), manyforge::core::CommandTargetId("gripper")}));
    EXPECT_EQ(node.target_effects[0].capability_id,
              CapabilityId("tool.gripper.command"));
    EXPECT_TRUE(node.target_effects[0].effectful);
    EXPECT_EQ(node.target_effects[1].target, (manyforge::core::ProgramTargetReferenceDTO{manyforge::core::ControllableComponentId("lite_gripper"), manyforge::core::CommandTargetId("gripper")}));
    EXPECT_EQ(node.target_effects[1].capability_id,
              CapabilityId("tool.gripper.feedback"));
    EXPECT_FALSE(node.target_effects[1].effectful);
  }
  EXPECT_EQ(table.nodes[0].parent_node_index, 2U);
  EXPECT_EQ(table.nodes[1].parent_node_index, 2U);
  EXPECT_EQ(table.nodes[2].parent_node_index, 3U);
  EXPECT_FALSE(table.nodes[3].parent_node_index.has_value());
  ASSERT_EQ(table.named_selections.size(), 3U);
  EXPECT_EQ(table.named_selections[0].group_id, "child_only");
  EXPECT_EQ(table.named_selections[0].node_indices,
            (std::vector<std::uint32_t>{0U}));
  EXPECT_EQ(table.named_selections[1].group_id, "mixed");
  EXPECT_EQ(table.named_selections[1].node_indices,
            (std::vector<std::uint32_t>{0U, 2U}));
  EXPECT_EQ(table.named_selections[2].group_id, "whole");
  EXPECT_EQ(table.named_selections[2].node_indices,
            (std::vector<std::uint32_t>{0U, 1U, 2U}));
  // Admission must join these exact current selections with the parent edges:
  // only `whole` holds the branch's full subtree and blocks its Continue child.
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
      verify_fixture_program(canonical_json(program), *bundle, root.path());
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
  const auto contract = verify_maintained_program(
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
  const auto contract = verify_fixture_program(
      canonical_json(CanonicalValue(source)), *bundle, root.path());
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
  EXPECT_EQ(verify_fixture_program(
                canonical_json(CanonicalValue(source)), *bundle, root.path())
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
        static_cast<void>(verify_fixture_program(
            canonical_json(with_tree(options, selection_node("root", invalid))),
            *bundle, root.path())),
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
        verify_fixture_program(canonical_json(expanded), *bundle, root.path()));
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
    static_cast<void>(verify_fixture_program(
        canonical_json(too_many_names), *bundle, root.path()));
    FAIL() << "excess group names accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(std::string(error.what()).find("group names exceed fixed bound"),
              std::string::npos);
  }
}

TEST(ProgramContractTest, CapturesOnlyCatalogQualifiedNodeTargetAssociations) {
  const auto bundle = maintained_motion_bundle();
  auto options = valid_options(*bundle);
  auto tree = selection_node("stop_arm", text_array({"robot_a"})).as_object();
  replace_member(tree, "node_id", CanonicalValue("kernel.stop_operation"));
  replace_member(tree, "parameters",
                 CanonicalValue(CanonicalValue::Object{
                     {"target", direct_target("lite6_robot", "arm")}}));
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
  const auto verified = verify_maintained_program(
      canonical_json(program_with_robot_a_group(CanonicalValue(tree))),
      *bundle);
  const auto &node = verified->execution_table().nodes.front();
  EXPECT_EQ(node.owner_api, "kernel.stop");
  ASSERT_EQ(node.target_effects.size(), 1U);
  EXPECT_EQ(node.target_effects.front().target, (manyforge::core::ProgramTargetReferenceDTO{manyforge::core::ControllableComponentId("lite6_robot"), manyforge::core::CommandTargetId("arm")}));
  EXPECT_EQ(node.target_effects.front().capability_id,
            CapabilityId("execution.stop.soft"));
  EXPECT_TRUE(node.target_effects.front().effectful);
  ASSERT_EQ(verified->value().target_requirements.size(), 1U);
  const auto &requirement = verified->value().target_requirements.front();
  EXPECT_EQ(requirement.target, node.target_effects.front().target);
  ASSERT_EQ(requirement.required_capabilities.size(), 1U);
  EXPECT_EQ(requirement.required_capabilities.front().capability_id,
            CapabilityId("execution.stop.soft"));
  EXPECT_EQ(requirement.required_capabilities.front().disposition,
            ProgramCapabilityDisposition::kRequired);
  // An unrelated component is refused before effect capture.
  replace_member(tree, "parameters", CanonicalValue(CanonicalValue::Object{
      {"target", direct_target("unrelated", "arm")}}));
  try {
    (void)verify_maintained_program(
        canonical_json(program_with_robot_a_group(CanonicalValue(tree))), *bundle);
    FAIL() << "missing component must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
                            [](const auto &row) {
                              return row.code == "component_reference_missing";
                            }));
  }
  replace_member(tree, "parameters", CanonicalValue(CanonicalValue::Object{
      {"target", CanonicalValue(CanonicalValue::Object{{"component_id", CanonicalValue("lite6_robot")}})}}));
  EXPECT_THROW(static_cast<void>(verify_maintained_program(
      canonical_json(program_with_robot_a_group(CanonicalValue(tree))), *bundle)), ProgramContractError);
  replace_member(tree, "parameters", CanonicalValue(CanonicalValue::Object{
      {"target", direct_target("lite6_robot", "arm")}}));
  replace_member(tree, "node_id", CanonicalValue("not_in_catalog"));
  EXPECT_THROW(
      static_cast<void>(verify_maintained_program(
          canonical_json(program_with_robot_a_group(CanonicalValue(tree))),
          *bundle)),
      ProgramContractError);
}

TEST(ProgramContractTest, CapturesMotionTargetEffectsWithoutDynamicScope) {
  const auto bundle = maintained_motion_bundle();
  auto options = valid_options(*bundle);
  auto tree = selection_node("move", CanonicalValue("all")).as_object();
  replace_member(tree, "node_id",
                 CanonicalValue("motion.move_to_joint_target"));
  replace_member(
      tree, "parameters",
      CanonicalValue(CanonicalValue::Object{
          {"target", direct_target("lite6_robot", "arm")},
          {"base_frame", direct_frame("lite6_robot", "base")},
          {"tcp_endpoint", arm_tcp_selector()},
          {"goal", CanonicalValue(CanonicalValue::Array{
                       CanonicalValue(CanonicalValue::Object{
                           {"joint_name", CanonicalValue("joint1")},
                           {"position_rad", CanonicalValue(0.0)}})})},
      }));
  const auto verify = [&] {
    return verify_maintained_program(
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
  EXPECT_TRUE(has(contract->execution_table(), "planning.motion.joint_goal"));
  EXPECT_TRUE(
      effectful(contract->execution_table(), "execution.trajectory.execute"));
  EXPECT_FALSE(
      effectful(contract->execution_table(), "state.observed.current"));
  // Narrow native capture proof, not a runnable Program: keep only literal
  // target/default-condition fields consumed at this boundary. Full motion
  // parameters would hit the independent YAML-node bound first. The effect
  // bound sits behind the node bound: 15 nodes per six-effect motion node hit
  // the 250,000 artifact node limit first, so the refusal names that limit.
  auto capture_only = tree;
  erase_member(capture_only, "execution_groups");
  replace_member(capture_only, "parameters",
                 CanonicalValue(CanonicalValue::Object{
                     {"target", direct_target("lite6_robot", "arm")}}));
  CanonicalValue::Array branches;
  for (std::size_t branch = 0U; branch < 2U; ++branch) {
    CanonicalValue::Array motions;
    for (std::size_t index = 0U; index < 8400U; ++index) {
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
        verify_maintained_program(canonical_json(many_effects), *bundle));
    FAIL() << "excess target associations accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(
        std::string(error.what()).find("artifact exceeds the v1 node-count limit"),
        std::string::npos)
        << error.what();
  }
  mutate_object_member(tree, "parameters", [](auto &parameters) {
    erase_member(parameters, "target");
  });
  tree.emplace_back(
      "value_sources",
      CanonicalValue(CanonicalValue::Object{
          {"target", CanonicalValue(CanonicalValue::Object{
                              {"kind", CanonicalValue("program_parameter")},
                              {"reference", CanonicalValue("chosen_target")}})},
      }));
  try {
    static_cast<void>(verify());
    FAIL() << "dynamic target selected an execution scope";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(std::string(error.what())
                  .find("cannot dynamically select runtime/binding semantics"),
              std::string::npos);
  }
}


[[nodiscard]] CanonicalValue
key_typed_program(const ProgramOptions &options, CanonicalValue tree,
                  CanonicalValue::Array extra_keys = {}) {
  const auto key = [](std::string name, std::string type) {
    return CanonicalValue(CanonicalValue::Object{
        {"key", CanonicalValue(std::move(name))},
        {"type", CanonicalValue(std::move(type))},
        {"access", CanonicalValue("read_write")},
        {"persistence", CanonicalValue("run")},
    });
  };
  const auto parameter = [](std::string name, std::string type,
                            CanonicalValue value) {
    return CanonicalValue(CanonicalValue::Object{
        {"parameter_id", CanonicalValue(std::move(name))},
        {"type", CanonicalValue(std::move(type))},
        {"default", std::move(value)},
    });
  };
  auto members = make_program_root(options).as_object();
  replace_member(members, "tree", std::move(tree));
  members.emplace_back(
      "blackboard",
      CanonicalValue(CanonicalValue::Object{
          {"namespaces", CanonicalValue(CanonicalValue::Array{})},
          {"keys", CanonicalValue([&] {
             CanonicalValue::Array keys{
                 key("count", "int64"), key("limit", "int64"),
                 key("place", "stored_pose"), key("ratio", "float64")};
             for (auto &extra : extra_keys) {
               keys.push_back(std::move(extra));
             }
             return keys;
           }())},
      }));
  members.emplace_back(
      "parameters",
      CanonicalValue(CanonicalValue::Array{
          parameter("start", "int64", CanonicalValue(1.0)),
          parameter("scale", "float64", CanonicalValue(0.5))}));
  return with_program_revision(CanonicalValue(std::move(members)));
}

[[nodiscard]] CanonicalValue::Object
key_typed_node(std::string node_id, CanonicalValue::Object parameters) {
  return CanonicalValue::Object{
      {"node_id", CanonicalValue(std::move(node_id))},
      {"instance_id", CanonicalValue("typed")},
      {"parameters", CanonicalValue(std::move(parameters))},
      {"children", CanonicalValue(CanonicalValue::Array{})},
  };
}

TEST(ProgramContractTest, SetTcpWholeValueSourceRequiresTcpEntryBlackboardKey) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto tcp_key = CanonicalValue(CanonicalValue::Object{
      {"key", CanonicalValue("selected_tcp")}, {"type", CanonicalValue("tcp_entry")},
      {"access", CanonicalValue("read")}, {"persistence", CanonicalValue("run")}});
  const auto make_program = [&](const char *kind, const char *reference,
                                const char *access = "read") {
    auto selected_key = tcp_key.as_object();
    replace_member(selected_key, "access", CanonicalValue(access));
    auto node = key_typed_node("motion.set_tcp",
                               {{"target", direct_target("lite6_arm", "arm")}});
    node.emplace_back("value_sources", CanonicalValue(CanonicalValue::Object{
        {"tcp", CanonicalValue(CanonicalValue::Object{
                    {"kind", CanonicalValue(kind)}, {"reference", CanonicalValue(reference)}})}}));
    auto program = key_typed_program(valid_options(*bundle), CanonicalValue(std::move(node)),
                                     {CanonicalValue(std::move(selected_key))}).as_object();
    for (auto &[name, value] : program) {
      if (name != "parameters") continue;
      auto parameters = value.as_array();
      parameters.emplace_back(CanonicalValue::Object{
          {"parameter_id", CanonicalValue("tcp_default")}, {"type", CanonicalValue("tcp_entry")},
          {"default", CanonicalValue(CanonicalValue::Object{
              {"target", direct_target("lite6_arm", "arm")},
              {"frame", direct_frame("lite6_arm", "flange")}})}});
      value = CanonicalValue(std::move(parameters));
    }
    return with_program_revision(CanonicalValue(std::move(program)));
  };
  // The context-free guard is deliberately still closed. Reaching it proves
  // the valid source passed the native structural prepass without sealing.
  EXPECT_THROW(static_cast<void>(verify_program_binding_contract(
                   canonical_json(make_program("blackboard_key", "selected_tcp")), *bundle)),
               manyforge::deployment::ProgramReferenceValidationIncomplete);
  for (const auto &[kind, reference] : {
           std::pair{"program_parameter", "tcp_default"},
           std::pair{"blackboard_key", "count"}}) {
    try {
      (void)verify_program_binding_contract(canonical_json(make_program(kind, reference)), *bundle);
      FAIL() << "invalid Set TCP value source was accepted";
    } catch (const ProgramContractError &error) {
      EXPECT_NE(std::string(error.what()).find("Set TCP requires a tcp_entry blackboard key"),
                std::string::npos);
    }
  }
  try {
    (void)verify_program_binding_contract(
        canonical_json(make_program("blackboard_key", "selected_tcp", "write")), *bundle);
    FAIL() << "write-only Set TCP source was accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_NE(std::string(error.what()).find("does not permit reads"),
              std::string::npos);
  }
}

TEST(ProgramContractTest, KeyTypedStoredPoseCapturesItsExactDirectFrame) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto pose = parse_strict_json_model_yaml(R"({
    "frame_ref":{"kind":"target_frame","frame":{"component_id":"lite6_arm","frame_id":"base"}},
    "position_m":[0.1,0.2,0.3],"orientation":{"xyzw":[0,0,0,1]}})").root();
  const auto node = key_typed_node("logic.set_value", {{"key", CanonicalValue("place")}, {"value", pose}});
  const auto program = key_typed_program(valid_options(*bundle), CanonicalValue(node));
  const auto contract = verify_fixture_program(canonical_json(program), *bundle, root.path());
  ASSERT_NE(contract, nullptr);
  const manyforge::core::ProgramFrameReferenceDTO expected{
      manyforge::core::ControllableComponentId("lite6_arm"), FrameRoleId("base")};
  EXPECT_EQ(contract->value().frame_requirements,
            (std::vector<manyforge::core::ProgramFrameReferenceDTO>{expected}));
  EXPECT_TRUE(contract->value().target_requirements.empty());
}

TEST(ProgramContractTest, KeyTypedStoredPoseMissingFrameIsRefusedBeforeCapture) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto pose = parse_strict_json_model_yaml(R"({
    "frame_ref":{"kind":"target_frame","frame":{"component_id":"lite6_arm","frame_id":"missing_frame"}},
    "position_m":[0.1,0.2,0.3],"orientation":{"xyzw":[0,0,0,1]}})").root();
  const auto node = key_typed_node("logic.set_value", {{"key", CanonicalValue("place")}, {"value", pose}});
  const auto program = key_typed_program(valid_options(*bundle), CanonicalValue(node));
  try {
    (void)verify_fixture_program(canonical_json(program), *bundle, root.path());
    FAIL() << "missing frame must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    ASSERT_EQ(error.diagnostics().size(), 1U);
    EXPECT_EQ(error.diagnostics().front().code, "frame_missing");
    EXPECT_EQ(error.diagnostics().front().location.path,
              "/tree/parameters/value/frame_ref/frame");
    EXPECT_EQ(error.diagnostics().front().location.node_instance_id, "typed");
  }
}

TEST(ProgramContractTest, PersistentPoseInitialMissingFrameIsRefusedBeforeCapture) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto pose = parse_strict_json_model_yaml(R"({
    "frame_ref":{"kind":"target_frame","frame":{"component_id":"lite6_arm","frame_id":"missing_frame"}},
    "position_m":[0,0,0],"orientation":{"xyzw":[0,0,0,1]}})").root();
  const auto key = CanonicalValue(CanonicalValue::Object{
      {"key", CanonicalValue("saved_pose")}, {"type", CanonicalValue("stored_pose")},
      {"access", CanonicalValue("read_write")}, {"persistence", CanonicalValue("persistent")},
      {"initial", pose}});
  const auto node = key_typed_node("logic.set_value", {{"key", CanonicalValue("count")},
                                                       {"value", CanonicalValue(1.0)}});
  const auto program = key_typed_program(valid_options(*bundle), CanonicalValue(node), {key});
  try {
    (void)verify_fixture_program(canonical_json(program), *bundle, root.path());
    FAIL() << "missing persistent initial frame must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    ASSERT_EQ(error.diagnostics().size(), 1U);
    EXPECT_EQ(error.diagnostics().front().code, "frame_missing");
    EXPECT_EQ(error.diagnostics().front().location.path,
              "/blackboard/keys/4/initial/frame_ref/frame");
  }
}

TEST(ProgramContractTest, PersistentPoseInitialCapturesItsExactFrame) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto pose = parse_strict_json_model_yaml(R"({
    "frame_ref":{"kind":"target_frame","frame":{"component_id":"lite6_arm","frame_id":"base"}},
    "position_m":[0,0,0],"orientation":{"xyzw":[0,0,0,1]}})").root();
  const auto key = CanonicalValue(CanonicalValue::Object{
      {"key", CanonicalValue("saved_pose")}, {"type", CanonicalValue("stored_pose")},
      {"access", CanonicalValue("read_write")}, {"persistence", CanonicalValue("persistent")},
      {"initial", pose}});
  const auto node = key_typed_node("logic.set_value", {{"key", CanonicalValue("count")},
                                                       {"value", CanonicalValue(1.0)}});
  const auto program = key_typed_program(valid_options(*bundle), CanonicalValue(node), {key});
  const auto contract = verify_fixture_program(canonical_json(program), *bundle, root.path());
  ASSERT_NE(contract, nullptr);
  const manyforge::core::ProgramFrameReferenceDTO expected{
      manyforge::core::ControllableComponentId("lite6_arm"), FrameRoleId("base")};
  EXPECT_EQ(contract->value().frame_requirements,
            (std::vector<manyforge::core::ProgramFrameReferenceDTO>{expected}));
}

TEST(ProgramContractTest, StoredPoseAllowedFrameMissingIsRefusedBeforeCapture) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto missing = parse_strict_json_model_yaml(R"({
    "kind":"target_frame","frame":{"component_id":"lite6_arm","frame_id":"missing_frame"}})").root();
  const auto key = CanonicalValue(CanonicalValue::Object{
      {"key", CanonicalValue("saved_pose")}, {"type", CanonicalValue("stored_pose")},
      {"access", CanonicalValue("read_write")}, {"persistence", CanonicalValue("run")},
      {"allowed_frames", CanonicalValue(CanonicalValue::Array{missing})}});
  const auto node = key_typed_node("logic.set_value", {{"key", CanonicalValue("count")},
                                                       {"value", CanonicalValue(1.0)}});
  const auto program = key_typed_program(valid_options(*bundle), CanonicalValue(node), {key});
  try {
    (void)verify_fixture_program(canonical_json(program), *bundle, root.path());
    FAIL() << "missing allowed frame must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    ASSERT_EQ(error.diagnostics().size(), 1U);
    EXPECT_EQ(error.diagnostics().front().code, "frame_missing");
    EXPECT_EQ(error.diagnostics().front().location.path,
              "/blackboard/keys/4/allowed_frames/0/frame");
  }
}

TEST(ProgramContractTest, SceneCreationRequiresDeclaredResourceInSharedValidator) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto &products = bundle->value().compiled_products.symbol_role_maps;
  ASSERT_EQ(products.size(), 1U);
  const manyforge::deployment::FilesystemVerifiedRetainedModelMapSource maps(root.path());
  const auto map = maps.get_verified(manyforge::deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(), products.front().product_id});
  ASSERT_TRUE(map.has_value());
  const manyforge::deployment::FilesystemCatalogArtifactStore catalog(root.path());
  for (const auto *node_id : {"scene.add_object", "scene.upsert_object"}) {
    SCOPED_TRACE(node_id);
    auto members = make_program_root(valid_options(*bundle)).as_object();
    replace_member(members, "tree", CanonicalValue(CanonicalValue::Object{
        {"node_id", CanonicalValue(node_id)},
        {"instance_id", CanonicalValue("create")},
        {"parameters", parse_strict_json_model_yaml(R"({
          "scene_resource_id":"missing_object",
          "shape":{"type":"box","box_dimensions_m":[0.1,0.1,0.1]},
          "pose":{"frame_ref":{"kind":"cell_frame"},"position_m":[0,0,0],
                  "orientation":{"xyzw":[0,0,0,1]}}})").root()},
        {"children", CanonicalValue(CanonicalValue::Array{})}}));
    const auto program = with_program_revision(CanonicalValue(std::move(members)));
    const auto diagnostics = manyforge::deployment::validate_program_references(
        program, false, *bundle, map, catalog);
    EXPECT_TRUE(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto &row) {
      return row.code == "scene_resource_missing" &&
             row.location.path == "/tree/parameters/scene_resource_id";
    }));
  }
}

TEST(ProgramContractTest, TypesKeyValueParametersByTheirLiteralKey) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto options = valid_options(*bundle);
  const auto verify = [&](const CanonicalValue::Object &node) {
    return verify_fixture_program(
        canonical_json(key_typed_program(options, CanonicalValue(node))),
        *bundle, root.path());
  };
  std::size_t rejected_case = 0U;
  const auto rejects = [&](const CanonicalValue::Object &node,
                           std::string_view reason) {
    SCOPED_TRACE(testing::Message() << "rejection case " << rejected_case++
                                    << ": " << reason);
    try {
      static_cast<void>(verify(node));
      ADD_FAILURE() << "accepted: " << reason;
    } catch (const ProgramContractError &error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
          << error.what();
    }
  };

  auto set_count = key_typed_node(
      "logic.set_value", {{"key", CanonicalValue("count")},
                          {"value", CanonicalValue(3.0)}});
  EXPECT_NO_THROW(static_cast<void>(verify(set_count)));

  auto fractional = set_count;
  mutate_object_member(fractional, "parameters", [](auto &parameters) {
    replace_member(parameters, "value", CanonicalValue(2.5));
  });
  rejects(fractional, "literal does not match key type 'int64'");

  auto boolean = set_count;
  mutate_object_member(boolean, "parameters", [](auto &parameters) {
    replace_member(parameters, "value", CanonicalValue(true));
  });
  rejects(boolean, "literal does not match key type 'int64'");

  const auto sourced = [&](std::string kind, std::string reference) {
    auto node = set_count;
    mutate_object_member(node, "parameters", [](auto &parameters) {
      erase_member(parameters, "value");
    });
    node.emplace_back(
        "value_sources",
        CanonicalValue(CanonicalValue::Object{
            {"value", CanonicalValue(CanonicalValue::Object{
                          {"kind", CanonicalValue(std::move(kind))},
                          {"reference", CanonicalValue(std::move(reference))},
                      })}}));
    return node;
  };
  EXPECT_NO_THROW(static_cast<void>(verify(sourced("program_parameter", "start"))));
  EXPECT_NO_THROW(static_cast<void>(verify(sourced("blackboard_key", "limit"))));
  rejects(sourced("program_parameter", "scale"),
          "source type 'float64' does not match the 'int64' type of key 'count'");
  rejects(sourced("blackboard_key", "ratio"),
          "source type 'float64' does not match the 'int64' type of key 'count'");
  rejects(sourced("blackboard_key", "absent"), "names no blackboard key");

  auto dynamic_key = set_count;
  mutate_object_member(dynamic_key, "parameters", [](auto &parameters) {
    erase_member(parameters, "key");
  });
  dynamic_key.emplace_back(
      "value_sources",
      CanonicalValue(CanonicalValue::Object{
          {"key", CanonicalValue(CanonicalValue::Object{
                      {"kind", CanonicalValue("program_parameter")},
                      {"reference", CanonicalValue("start")},
                  })}}));
  rejects(dynamic_key, "requires a literal key parameter: key");

  auto undeclared = set_count;
  mutate_object_member(undeclared, "parameters", [](auto &parameters) {
    replace_member(parameters, "key", CanonicalValue("missing"));
  });
  rejects(undeclared, "names no declared blackboard key: value");

  // A stored_pose key needs a pose literal: a scalar here would be refused by
  // the pose capture before the admitted-type check this case tests.
  const auto pose = parse_strict_json_model_yaml(R"({
    "frame_ref":{"kind":"target_frame","frame":{"component_id":"lite6_arm","frame_id":"base"}},
    "position_m":[0,0,0],"orientation":{"xyzw":[0,0,0,1]}})").root();
  rejects(key_typed_node("logic.check_value",
                         {{"key", CanonicalValue("place")},
                          {"value", pose}}),
          "blackboard key type 'stored_pose' is not admitted by the node");
}

[[nodiscard]] CanonicalValue expression(std::string text) {
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("expression")},
      {"expression", CanonicalValue(std::move(text))},
  });
}

[[nodiscard]] CanonicalValue computed_key(std::string name, std::string text,
                                          CanonicalValue::Object extra = {}) {
  CanonicalValue::Object row{
      {"key", CanonicalValue(std::move(name))},
      {"type", CanonicalValue("int64")},
      {"access", CanonicalValue("read")},
      {"persistence", CanonicalValue("run")},
      {"expression", CanonicalValue(std::move(text))},
  };
  for (auto &[field, value] : extra) {
    bool replaced = false;
    for (auto &[existing, current] : row) {
      if (existing == field) {
        current = value;
        replaced = true;
      }
    }
    if (!replaced) {
      row.emplace_back(field, value);
    }
  }
  return CanonicalValue(std::move(row));
}

TEST(ProgramContractTest, ChecksPathKeyedValueSourcesAndFieldExpressions) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto options = valid_options(*bundle);
  const auto node = [](std::string key, CanonicalValue value,
                       CanonicalValue::Object sources) {
    auto result = key_typed_node(
        "logic.set_value",
        {{"key", CanonicalValue(std::move(key))}, {"value", std::move(value)}});
    result.emplace_back("value_sources", CanonicalValue(std::move(sources)));
    return result;
  };
  const auto verify = [&](const CanonicalValue::Object &tree) {
    return verify_fixture_program(
        canonical_json(key_typed_program(options, CanonicalValue(tree))),
        *bundle, root.path());
  };
  const auto rejects = [&](const CanonicalValue::Object &tree,
                           std::string_view reason) {
    try {
      static_cast<void>(verify(tree));
      ADD_FAILURE() << "accepted: " << reason;
    } catch (const ProgramContractError &error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
          << error.what();
    }
  };
  const CanonicalValue pose(CanonicalValue::Object{
      {"frame_ref",
       CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue("cell_frame")}})},
      {"position_m", CanonicalValue(CanonicalValue::Array{
                         CanonicalValue(0.1), CanonicalValue(0.2),
                         CanonicalValue(0.3)})},
      {"orientation",
       CanonicalValue(CanonicalValue::Object{
           {"rpy_deg", CanonicalValue(CanonicalValue::Array{
                           CanonicalValue(0.0), CanonicalValue(0.0),
                           CanonicalValue(0.0)})}})},
  });

  EXPECT_NO_THROW(static_cast<void>(verify(node(
      "count", CanonicalValue(1.0), {{"value", expression("key.count + param.start")}}))));
  rejects(node("count", CanonicalValue(1.0), {{"value", expression("key.count / 2")}}),
          "value_sources.value.expression: destination_type_mismatch");
  rejects(node("count", CanonicalValue(1.0), {{"value", expression("key.absent")}}),
          "unknown_name at character 0: key.absent is not declared");
  EXPECT_NO_THROW(static_cast<void>(verify(node(
      "place", pose,
      {{"value.orientation.rpy_deg[2]", expression("param.scale * 90")},
       {"value.position_m[2]", expression("param.scale")}}))));
  // As in Python: a path names a value of the literal before its schema.
  rejects(node("place", pose,
               {{"value.orientation.xyzw[0]", expression("1.0")}}),
          "names no value in the node's parameters");
  rejects(node("place", pose, {{"value.frame_ref.kind", expression("\"x\"")}}),
          "nested text, choice and identifier fields stay literal");
  rejects(node("count", CanonicalValue(1.0), {{"value.x", expression("1")}}),
          "int64 values have no fields");
  rejects(node("count", CanonicalValue(1.0), {{"absent", expression("1")}}),
          "supplies unknown catalog parameter 'absent'");
  rejects(node("count", CanonicalValue(1.0), {{"value[01]", expression("1")}}),
          "path_index_invalid");
  try {
    static_cast<void>(verify(node("count", CanonicalValue(1.0),
                                  {{"value[01]", expression("1")}})));
    FAIL() << "malformed value path was accepted";
  } catch (const ProgramContractError &error) {
    EXPECT_EQ(error.code(), "path_index_invalid");
    EXPECT_NE(std::string(error.what()).find("path_index_invalid"), std::string::npos);
    const std::exception &base = error;
    EXPECT_NE(dynamic_cast<const std::invalid_argument *>(&base), nullptr);
  }
  rejects(node("place", pose,
               {{"value", CanonicalValue(CanonicalValue::Object{
                              {"kind", CanonicalValue("blackboard_key")},
                              {"reference", CanonicalValue("place")}})},
                {"value.position_m[0]", expression("1.0")}}),
          "overlaps the value_sources entry 'value'");
}

[[nodiscard]] CanonicalValue load_conformance(const char *path) {
  std::ifstream stream(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  return parse_strict_json_model_yaml(text).root();
}

// The value-source refusals shared with the Python Program validator
// (manyforge_behavior/tests/test_program_expressions.py).
[[nodiscard]] const CanonicalValue::Object &value_source_conformance() {
  static const CanonicalValue root =
      load_conformance(MANYFORGE_VALUE_SOURCE_CONFORMANCE);
  return root.as_object();
}

[[nodiscard]] const CanonicalValue &
conformance_member(const CanonicalValue::Object &object, std::string_view name) {
  for (const auto &[key, value] : object) {
    if (key == name) {
      return value;
    }
  }
  throw std::logic_error("conformance entry lacks " + std::string(name));
}

[[nodiscard]] CanonicalValue
conformance_program(const ProgramOptions &options,
                    const CanonicalValue::Object &test_case) {
  const auto &file = value_source_conformance();
  auto node = conformance_member(
                  conformance_member(file, "nodes").as_object(),
                  conformance_member(test_case, "node").as_string())
                  .as_object();
  for (const auto &[key, value] : test_case) {
    if (key != "parameters") {
      continue;
    }
    mutate_object_member(node, "parameters", [&](auto &parameters) {
      for (const auto &[name, literal] : value.as_object()) {
        erase_member(parameters, name);
        parameters.emplace_back(name, literal);
      }
    });
  }
  node.emplace_back("value_sources",
                    conformance_member(test_case, "value_sources"));
  node.emplace_back("children", CanonicalValue(CanonicalValue::Array{}));
  auto members = make_program_root(options).as_object();
  replace_member(members, "tree", CanonicalValue(std::move(node)));
  members.emplace_back("parameters", conformance_member(file, "parameters"));
  members.emplace_back(
      "blackboard",
      CanonicalValue(CanonicalValue::Object{
          {"namespaces", CanonicalValue(CanonicalValue::Array{})},
          {"keys", conformance_member(file, "blackboard_keys")},
      }));
  return with_program_revision(CanonicalValue(std::move(members)));
}

TEST(ProgramContractTest, RefusesTheSharedValueSourceConformanceCases) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto options = valid_options(*bundle);
  const auto &file = value_source_conformance();
  const auto verify = [&](const CanonicalValue &test_case) {
    return verify_fixture_program(
        canonical_json(conformance_program(options, test_case.as_object())),
        *bundle, root.path());
  };
  for (const auto &test_case : conformance_member(file, "accepted").as_array()) {
    SCOPED_TRACE(conformance_member(test_case.as_object(), "name").as_string());
    try {
      static_cast<void>(verify(test_case));
    } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &refusal) {
      // The Lite 6 fixture cell has no usable model query, so a model node
      // reports only that capability; its value sources are accepted.
      for (const auto &diagnostic : refusal.diagnostics()) {
        EXPECT_EQ(diagnostic.code, "cell_capability_missing");
        const auto *capability = diagnostic.payload.find_object_member("capability_id");
        ASSERT_NE(capability, nullptr);
        EXPECT_EQ(capability->as_string(), "assistant.model.query");
      }
    }
  }
  std::size_t refused = 0U;
  for (const auto &test_case : conformance_member(file, "refused").as_array()) {
    const auto &reason =
        conformance_member(test_case.as_object(), "reason").as_string();
    SCOPED_TRACE(conformance_member(test_case.as_object(), "name").as_string());
    try {
      static_cast<void>(verify(test_case));
      ADD_FAILURE() << "accepted: " << reason;
    } catch (const ProgramContractError &error) {
      ++refused;
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
          << error.what();
    }
  }
  EXPECT_GT(refused, 0U);
}

// The pose-value refusals shared with the Python Program validator
// (manyforge_behavior/tests/test_program_expressions.py).
[[nodiscard]] CanonicalValue
pose_conformance_program(const ProgramOptions &options,
                         const CanonicalValue::Object &file,
                         const CanonicalValue::Object &test_case) {
  auto node = conformance_member(file, "node").as_object();
  CanonicalValue::Array keys =
      conformance_member(file, "blackboard_keys").as_array();
  for (const auto &[name, value] : test_case) {
    if (name == "value") {
      mutate_object_member(node, "parameters", [&](auto &parameters) {
        replace_member(parameters, "value", value);
      });
    } else if (name == "keys") {
      keys.insert(keys.end(), value.as_array().begin(), value.as_array().end());
    }
  }
  std::sort(keys.begin(), keys.end(), [](const auto &left, const auto &right) {
    return conformance_member(left.as_object(), "key").as_string() <
           conformance_member(right.as_object(), "key").as_string();
  });
  node.emplace_back("children", CanonicalValue(CanonicalValue::Array{}));
  auto members = make_program_root(options).as_object();
  replace_member(members, "tree",
                 CanonicalValue(CanonicalValue::Object{
                     {"node_id", CanonicalValue("sequence")},
                     {"instance_id", CanonicalValue("root")},
                     {"parameters", CanonicalValue(CanonicalValue::Object{})},
                     {"children", CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue(std::move(node))})},
                 }));
  members.emplace_back("scene", conformance_member(file, "scene"));
  members.emplace_back(
      "blackboard",
      CanonicalValue(CanonicalValue::Object{
          {"namespaces", CanonicalValue(CanonicalValue::Array{})},
          {"keys", CanonicalValue(std::move(keys))},
      }));
  return with_program_revision(CanonicalValue(std::move(members)));
}

TEST(ProgramContractTest, RefusesTheSharedPoseValueConformanceCases) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto options = valid_options(*bundle);
  static const CanonicalValue file =
      load_conformance(MANYFORGE_POSE_VALUE_CONFORMANCE);
  const auto verify = [&](const CanonicalValue &test_case) {
    return verify_fixture_program(
        canonical_json(pose_conformance_program(options, file.as_object(),
                                                test_case.as_object())),
        *bundle, root.path());
  };
  for (const auto &test_case :
       conformance_member(file.as_object(), "accepted").as_array()) {
    SCOPED_TRACE(conformance_member(test_case.as_object(), "name").as_string());
    // Pose-value shape is checked before reference validation, so a shape
    // refusal stays fatal here. The Lite6 bundle carries no `gantry` or
    // `camera` component and no `lite6_robot` (its arm is `lite6_arm`), so the
    // direct references of these accepted cases are refused by the shared
    // reference validator, which owns their resolution.
    try {
      static_cast<void>(verify(test_case));
    } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &) {
    }
  }
  std::size_t refused = 0U;
  for (const auto &test_case :
       conformance_member(file.as_object(), "refused").as_array()) {
    const auto &reason =
        conformance_member(test_case.as_object(), "reason").as_string();
    SCOPED_TRACE(conformance_member(test_case.as_object(), "name").as_string());
    try {
      static_cast<void>(verify(test_case));
      ADD_FAILURE() << "accepted: " << reason;
    } catch (const ProgramContractError &error) {
      ++refused;
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
          << error.what();
    }
  }
  EXPECT_GT(refused, 0U);
}

TEST(ProgramContractTest, ChecksComputedKeysAndRefusesTheirCycles) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto options = valid_options(*bundle);
  const auto tree = CanonicalValue(key_typed_node(
      "logic.set_value",
      {{"key", CanonicalValue("count")}, {"value", CanonicalValue(1.0)}}));
  const auto verify = [&](CanonicalValue::Array keys) {
    return verify_fixture_program(
        canonical_json(key_typed_program(options, tree, std::move(keys))),
        *bundle, root.path());
  };
  const auto rejects = [&](CanonicalValue::Array keys, std::string_view reason) {
    try {
      static_cast<void>(verify(std::move(keys)));
      ADD_FAILURE() << "accepted: " << reason;
    } catch (const ProgramContractError &error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
          << error.what();
    }
  };

  EXPECT_NO_THROW(static_cast<void>(verify(
      {computed_key("z_next", "key.count + param.start"),
       computed_key("z_twice", "key.z_next * 2")})));
  rejects({computed_key("z_a", "key.z_b + 1"), computed_key("z_b", "key.z_a + 1")},
          "computed keys read each other in a cycle: z_a -> z_b -> z_a");
  rejects({computed_key("z_self", "key.z_self")},
          "cycle: z_self -> z_self");
  rejects({computed_key("z_next", "1", {{"initial", CanonicalValue(1.0)}})},
          "a computed key has no initial value");
  rejects({computed_key("z_next", "1",
                        {{"persistence", CanonicalValue("persistent")}})},
          "a computed key cannot be persistent");
  rejects({computed_key("z_next", "1", {{"access", CanonicalValue("read_write")}})},
          "declare access read");
  rejects({computed_key("z_next", "1", {{"type", CanonicalValue("stored_pose")}})},
          "a computed key must be bool, int64, float64 or string");
  rejects({computed_key("z_next", "param.unknown")},
          "expression: unknown_name at character 0");
}

TEST(ProgramContractTest,
     CapturesExactBlackboardDeclarationTypesWithoutSelectingSubscriptions) {
  ScopedTrustedRoot root;
  const auto bundle = make_bundle(root.path());
  const auto program = key_typed_program(
      valid_options(*bundle),
      CanonicalValue(
          key_typed_node("logic.set_value", {{"key", CanonicalValue("count")},
                                             {"value", CanonicalValue(1.0)}})),
      {computed_key("flag", "true", {{"type", CanonicalValue("bool")}}),
       computed_key("label", "\"ready\"",
                    {{"type", CanonicalValue("string")}})});
  const auto bytes = canonical_json(program);
  const auto verified = verify_fixture_program(bytes, *bundle, root.path());
  EXPECT_EQ(verified->blackboard_key_declarations(),
            (std::vector<manyforge::core::ProgramBlackboardKeyDeclarationDTO>{
                {"count", "int64"},
                {"flag", "bool"},
                {"label", "string"},
                {"limit", "int64"},
                {"place", "stored_pose"},
                {"ratio", "float64"}}));
  EXPECT_EQ(verified->normalized_program_root(), program);
  EXPECT_EQ(verified->canonical_program_bytes(), bytes);
}

TEST(ProgramContractTest,
     ImplicitMotionOutcomesAreReadableWithoutChangingAuthoredIdentity) {
  const auto bundle = maintained_motion_bundle();
  for (const std::string node_id :
       {"motion.move_to_joint_target", "motion.move_to_pose",
        "motion.follow_cartesian_path"}) {
    for (const auto &instance : {std::string("move"), std::string(128U, 'm')}) {
      const auto outcome = "node_outcome." + instance;
      auto members = automatic_retry_program(*bundle, node_id).as_object();
      auto motion = conformance_member(members, "tree").as_object();
      replace_member(motion, "instance_id", CanonicalValue(instance));
      auto reader = key_typed_node("logic.check_value",
                                   {{"key", CanonicalValue(outcome)},
                                    {"value", CanonicalValue("completed")}});
      replace_member(
          members, "tree",
          selection_node("root", CanonicalValue("inherit"),
                         {CanonicalValue(motion), CanonicalValue(reader)}));
      members.emplace_back(
          "blackboard",
          CanonicalValue(CanonicalValue::Object{
              {"namespaces", CanonicalValue(CanonicalValue::Array{})},
              {"keys", CanonicalValue(CanonicalValue::Array{computed_key(
                           "finished", "key." + outcome + " == \"completed\"",
                           {{"type", CanonicalValue("bool")}})})}}));
      auto program = with_program_revision(CanonicalValue(std::move(members)));
      const auto bytes = canonical_json(program);
      const auto contract = verify_maintained_program(bytes, *bundle);
      EXPECT_EQ(contract->canonical_program_bytes(), bytes);
      EXPECT_EQ(contract->normalized_program_root(), program);
      // A motion target also declares its implicit Active TCP key; the
      // declarations are sorted by key name.
      const auto active_tcp = *manyforge::core::ActiveTcpBlackboardKey(
          manyforge::core::ProgramTargetReferenceDTO{
              manyforge::core::ControllableComponentId("lite6_robot"),
              manyforge::core::CommandTargetId("arm")});
      EXPECT_EQ(
          contract->blackboard_key_declarations(),
          (std::vector<manyforge::core::ProgramBlackboardKeyDeclarationDTO>{
              {active_tcp, "tcp_entry"}, {"finished", "bool"}, {outcome, "string"}}));
      // The computed read and literal key read use the same implicit
      // declaration.
      auto sourced = reader;
      mutate_object_member(sourced, "parameters", [](auto &parameters) {
        erase_member(parameters, "value");
      });
      sourced.emplace_back(
          "value_sources",
          CanonicalValue(CanonicalValue::Object{
              {"value", CanonicalValue(CanonicalValue::Object{
                            {"kind", CanonicalValue("blackboard_key")},
                            {"reference", CanonicalValue(outcome)}})}}));
      auto source_members = program.as_object();
      replace_member(source_members, "tree",
                     selection_node("root", CanonicalValue("inherit"),
                                    {CanonicalValue(motion),
                                     CanonicalValue(std::move(sourced))}));
      EXPECT_NO_THROW(static_cast<void>(verify_maintained_program(
          canonical_json(
              with_program_revision(CanonicalValue(std::move(source_members)))),
          *bundle)));
    }
  }
}

TEST(ProgramContractTest, RefusesAuthoredOutcomeDeclarationsAndCatalogWrites) {
  const auto bundle = maintained_motion_bundle();
  const auto base =
      automatic_retry_program(*bundle, "motion.move_to_joint_target");
  const auto rejects = [&](CanonicalValue::Object members,
                           std::string_view reason) {
    try {
      static_cast<void>(verify_maintained_program(
          canonical_json(
              with_program_revision(CanonicalValue(std::move(members)))),
          *bundle));
      ADD_FAILURE() << "accepted reserved outcome authoring";
    } catch (const ProgramContractError &error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos)
          << error.what();
    }
  };
  auto namespaces = base.as_object();
  namespaces.emplace_back(
      "blackboard",
      CanonicalValue(CanonicalValue::Object{
          {"namespaces",
           CanonicalValue(
               CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                   {"namespace_id", CanonicalValue("node_outcome")},
                   {"owner", CanonicalValue("program")}})})},
          {"keys", CanonicalValue(CanonicalValue::Array{})}}));
  rejects(std::move(namespaces), "node_outcome is reserved");
  auto keys = base.as_object();
  keys.emplace_back(
      "blackboard",
      CanonicalValue(CanonicalValue::Object{
          {"namespaces", CanonicalValue(CanonicalValue::Array{})},
          {"keys", CanonicalValue(CanonicalValue::Array{
                       CanonicalValue(CanonicalValue::Object{
                           {"key", CanonicalValue("node_outcome.move")},
                           {"type", CanonicalValue("string")},
                           {"access", CanonicalValue("read")},
                           {"persistence", CanonicalValue("run")}})})}}));
  rejects(std::move(keys), "cannot be authored");
  auto writer = base.as_object();
  replace_member(
      writer, "tree",
      selection_node(
          "root", CanonicalValue("inherit"),
          {conformance_member(base.as_object(), "tree"),
           CanonicalValue(key_typed_node(
               "logic.set_value", {{"key", CanonicalValue("node_outcome.move")},
                                   {"value", CanonicalValue("failed")}}))}));
  rejects(std::move(writer), "cannot be written by catalog bindings");
  auto absent = base.as_object();
  replace_member(
      absent, "tree",
      selection_node("root", CanonicalValue("inherit"),
                     {conformance_member(base.as_object(), "tree"),
                      CanonicalValue(key_typed_node(
                          "logic.check_value",
                          {{"key", CanonicalValue("node_outcome.other")},
                           {"value", CanonicalValue("failed")}}))}));
  rejects(std::move(absent), "names no declared blackboard key");
}

TEST(ProgramContractTest,
     SignalMotionOutcomesHaveImplicitReadOnlyDeclarations) {
  const auto bundle = maintained_motion_bundle();
  for (const std::string node_id :
       {"motion.move_until_signal", "motion.probe_search"}) {
    auto members =
        automatic_retry_program(*bundle, "motion.move_to_pose").as_object();
    auto signal = conformance_member(members, "tree").as_object();
    replace_member(signal, "node_id", CanonicalValue(node_id));
    mutate_object_member(signal, "parameters", [&](auto &parameters) {
      mutate_object_member(parameters, "goal", [](auto &goal) {
        goal.emplace_back("frame_ref",
                          CanonicalValue(CanonicalValue::Object{
                              {"kind", CanonicalValue("target_frame")},
                              {"frame", direct_frame("lite6_robot", "base")}}));
      });
      parameters.emplace_back("interrupt_condition", parse_strict_json_model_yaml(
          R"({"compare":{"operand":{"key":"node_outcome.move"},"op":"==","value":"none"}})").root());
      parameters.emplace_back("stop_mode", CanonicalValue("spring_back_hold"));
      if (node_id == "motion.probe_search")
        parameters.emplace_back("contact_policy_id",
                                CanonicalValue("contact_policy"));
    });
    auto reader = key_typed_node("logic.check_value",
                                 {{"key", CanonicalValue("node_outcome.move")},
                                  {"value", CanonicalValue("signal_trip")}});
    replace_member(
        members, "tree",
        selection_node("root", CanonicalValue("inherit"),
                       {CanonicalValue(signal), CanonicalValue(reader)}));
    members.emplace_back(
        "blackboard",
        CanonicalValue(CanonicalValue::Object{
            {"namespaces", CanonicalValue(CanonicalValue::Array{})},
            {"keys",
             CanonicalValue(CanonicalValue::Array{computed_key(
                 "no_trip", "key.node_outcome.move == \"no_signal_trip\"",
                 {{"type", CanonicalValue("bool")}})})}}));
    const auto program = with_program_revision(CanonicalValue(members));
    const auto verified =
        verify_maintained_program(canonical_json(program), *bundle);
    EXPECT_EQ(verified->normalized_program_root(), program);
    // This verifier supplies readable types, not an outcome value or an
    // activated interrupt. Runtime availability keeps its existing owner.
    mutate_object_member(reader, "parameters", [](auto &parameters) {
      replace_member(parameters, "value", CanonicalValue("no_signal_trip"));
    });
    replace_member(reader, "node_id", CanonicalValue("logic.set_value"));
    replace_member(
        members, "tree",
        selection_node("root", CanonicalValue("inherit"),
                       {CanonicalValue(signal), CanonicalValue(reader)}));
    EXPECT_THROW(static_cast<void>(verify_maintained_program(
                     canonical_json(with_program_revision(
                         CanonicalValue(std::move(members)))),
                     *bundle)),
                 ProgramContractError);
  }
}

TEST(ProgramContractTest,
     SystemNamespaceNamesAreRefusedForParametersKeysAndNamespaceDeclarations) {
  const auto bundle = maintained_motion_bundle();
  const auto base =
      automatic_retry_program(*bundle, "motion.move_to_joint_target");
  for (const auto *name : {"node_outcome", "node_outcome.move",
                           "interrupt_state", "interrupt_state.guard"}) {
    for (const auto *kind : {"parameter", "key", "namespace"}) {
      SCOPED_TRACE(name);
      SCOPED_TRACE(kind);
      auto members = base.as_object();
      if (std::string_view(kind) == "parameter") {
        members.emplace_back(
            "parameters",
            CanonicalValue(CanonicalValue::Array{CanonicalValue(
                CanonicalValue::Object{{"parameter_id", CanonicalValue(name)},
                                       {"type", CanonicalValue("string")},
                                       {"default", CanonicalValue("none")}})}));
      } else {
        CanonicalValue::Array namespaces, keys;
        if (std::string_view(kind) == "namespace")
          namespaces.emplace_back(
              CanonicalValue::Object{{"namespace_id", CanonicalValue(name)},
                                     {"owner", CanonicalValue("program")}});
        else
          keys.emplace_back(
              CanonicalValue::Object{{"key", CanonicalValue(name)},
                                     {"type", CanonicalValue("string")},
                                     {"access", CanonicalValue("read_write")},
                                     {"persistence", CanonicalValue("run")},
                                     {"initial", CanonicalValue("none")}});
        members.emplace_back(
            "blackboard",
            CanonicalValue(CanonicalValue::Object{
                {"namespaces", CanonicalValue(std::move(namespaces))},
                {"keys", CanonicalValue(std::move(keys))}}));
      }
      try {
        (void)verify_maintained_program(
            canonical_json(
                with_program_revision(CanonicalValue(std::move(members)))),
            *bundle);
        ADD_FAILURE() << "authored system name was admitted";
      } catch (const ProgramContractError &error) {
        EXPECT_NE(std::string(error.what()).find("reserved_system_namespace"),
                  std::string::npos)
            << error.what();
      }
    }
  }
}

TEST(ProgramContractTest,
     NodeOutcomeNoneIsReadableAsLiteralAndExpressionWithoutAuthoredInitial) {
  const auto bundle = maintained_motion_bundle();
  auto members =
      automatic_retry_program(*bundle, "motion.move_to_pose").as_object();
  auto reader = key_typed_node("logic.check_value",
                               {{"key", CanonicalValue("node_outcome.move")},
                                {"value", CanonicalValue("none")}});
  replace_member(members, "tree",
                 selection_node("root", CanonicalValue("inherit"),
                                {conformance_member(members, "tree"),
                                 CanonicalValue(reader)}));
  members.emplace_back(
      "blackboard",
      CanonicalValue(CanonicalValue::Object{
          {"namespaces", CanonicalValue(CanonicalValue::Array{})},
          {"keys", CanonicalValue(CanonicalValue::Array{computed_key(
                       "started", "key.node_outcome.move != \"none\"",
                       {{"type", CanonicalValue("bool")}})})}}));
  const auto program =
      with_program_revision(CanonicalValue(std::move(members)));
  const auto verified =
      verify_maintained_program(canonical_json(program), *bundle);
  EXPECT_EQ(verified->normalized_program_root(), program);
  const auto &keys = verified->blackboard_key_declarations();
  const auto system =
      std::find_if(keys.begin(), keys.end(), [](const auto &key) {
        return key.name == "node_outcome.move";
      });
  ASSERT_NE(system, keys.end());
  EXPECT_EQ(system->declared_type, "string");
  EXPECT_FALSE(system->has_declared_initial);
}


TEST(ProgramContractTest, NodeMockSimulationAcceptsCatalogLiteralsAndRefusesDynamicSources) {
  const auto bundle = maintained_motion_bundle();
  auto options = valid_options(*bundle);
  const auto catalog = manyforge::deployment::ResolveCompiledIoSignalCatalog(bundle->value().cell_graph);
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  const auto input = std::find_if(catalog.value().begin(), catalog.value().end(), [](const auto &row) {
    return row.descriptor.direction == manyforge::core::IoSignalDirection::kInput &&
           row.descriptor.value_type == manyforge::core::IoSignalValueType::kBoolean;
  });
  ASSERT_NE(input, catalog.value().end());
  const CanonicalValue reference(CanonicalValue::Object{
      {"component_id", CanonicalValue(input->descriptor.signal_id.controllable_component_id.value())},
      {"signal_name", CanonicalValue(input->descriptor.signal_id.name)}});
  const CanonicalValue rows(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
      {"signal", reference},
      {"start_value", CanonicalValue(false)},
      {"change", CanonicalValue(CanonicalValue::Object{
          {"after_ms", CanonicalValue(0.0)}, {"value", CanonicalValue(true)}})}})});
  const CanonicalValue::Object literal_node{
      {"node_id", CanonicalValue("io.check_input")},
      {"instance_id", CanonicalValue("reader")},
      {"parameters", CanonicalValue(CanonicalValue::Object{
          {"signal", reference},
          {"expected_value", CanonicalValue(true)},
          {"mock_simulation", rows}})},
      {"children", CanonicalValue(CanonicalValue::Array{})}};
  // This control must succeed with the actual embedded catalog. An unknown
  // parameter refusal from an older catalog cannot qualify the negatives.
  const auto literal = with_tree(options, CanonicalValue(literal_node));
  const auto verified = verify_maintained_program(canonical_json(literal), *bundle);
  ASSERT_NE(verified, nullptr);
  EXPECT_EQ(verified->normalized_program_root(), literal);
  const manyforge::core::ProgramSignalReferenceDTO expected_signal{
      input->descriptor.signal_id.controllable_component_id,
      input->descriptor.signal_id.name};
  EXPECT_EQ(verified->value().signal_requirements,
            (std::vector<manyforge::core::ProgramSignalReferenceDTO>{expected_signal}));
  ASSERT_EQ(verified->execution_table().nodes.size(), 1U);
  EXPECT_EQ(verified->execution_table().nodes.front().signal_references,
            (std::vector<manyforge::core::ProgramSignalReferenceDTO>{expected_signal}));
  auto invalid_node = literal_node;
  mutate_object_member(invalid_node, "parameters", [&](auto &parameters) {
    const auto bad_reference = CanonicalValue(CanonicalValue::Object{
        {"component_id", CanonicalValue(input->descriptor.signal_id.controllable_component_id.value())},
        {"signal_name", CanonicalValue("missing_fixture_signal")}});
    replace_member(parameters, "mock_simulation", CanonicalValue(CanonicalValue::Array{
        CanonicalValue(CanonicalValue::Object{{"signal", bad_reference},
                                               {"start_value", CanonicalValue(false)}})}));
  });
  try {
    (void)verify_maintained_program(
        canonical_json(with_tree(options, CanonicalValue(std::move(invalid_node)))), *bundle);
    FAIL() << "missing nested signal must be refused before effect capture";
  } catch (const manyforge::deployment::ProgramReferenceValidationRefusal &error) {
    EXPECT_TRUE(std::any_of(error.diagnostics().begin(), error.diagnostics().end(),
                            [](const auto &row) {
      return row.code == "signal_missing" &&
             row.location.path == "/tree/parameters/mock_simulation/0/signal";
    }));
  }
  for (const std::string path : {"mock_simulation", "mock_simulation[0].start_value",
                                  "mock_simulation[0].change.value"}) {
    SCOPED_TRACE(path);
    auto node = literal_node;
    node.emplace_back("value_sources", CanonicalValue(CanonicalValue::Object{
        {path, CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("expression")},
            {"expression", CanonicalValue("false")}})}}));
    try {
      (void)verify_maintained_program(
          canonical_json(with_tree(options, CanonicalValue(std::move(node)))), *bundle);
      ADD_FAILURE() << "node Mock simulation admitted a dynamic source at " << path;
    } catch (const ProgramContractError& error) {
      const std::string message(error.what());
      EXPECT_NE(message.find(path), std::string::npos) << message;
      EXPECT_NE(message.find("cannot dynamically select runtime/binding semantics"),
                std::string::npos) << message;
      EXPECT_EQ(message.find("unknown catalog parameter"), std::string::npos) << message;
    }
  }
}

} // namespace
