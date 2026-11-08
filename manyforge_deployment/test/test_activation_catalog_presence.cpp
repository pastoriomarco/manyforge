#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/prepared_catalog_source.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>

#include "activation_deployment_intent_internal.hpp"
#include "content_identity_internal.hpp"
#include "maintained_activation_fixture.hpp"
#include "prepared_activation_materialization_internal.hpp"

namespace {

namespace fs = std::filesystem;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;
namespace detail = manyforge::deployment::detail;
using deployment::CanonicalValue;

// The Apply path qualifies the maintained catalog by presence:
// extra records and extra offers are accepted, while each required lifecycle
// provider must still carry its Stop contract roles. Each test runs the normal
// activation materializer against a private copy of the catalog resources.
class CatalogCopy final {
public:
  explicit CatalogCopy(std::string_view name)
      : root_(fs::temp_directory_path() /
              ("manyforge_catalog_presence_" + std::string(name))) {
    fs::remove_all(root_);
    fs::create_directories(root_ / "manyforge_deployment");
    fs::copy(fs::path(MANYFORGE_WORKSPACE_ROOT) / "manyforge_deployment/resources",
             root_ / "manyforge_deployment/resources",
             fs::copy_options::recursive);
  }
  ~CatalogCopy() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  CatalogCopy(const CatalogCopy &) = delete;
  CatalogCopy &operator=(const CatalogCopy &) = delete;

  [[nodiscard]] const fs::path &root() const { return root_; }
  [[nodiscard]] fs::path catalog(std::string_view kind) const {
    return root_ / "manyforge_deployment/resources/maintained/catalog" /
           std::string(kind);
  }

private:
  fs::path root_;
};

std::string Read(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::logic_error("catalog presence fixture missing: " + path.string());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void Write(const fs::path &path, const std::string &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << bytes;
  if (!output)
    throw std::logic_error("catalog presence fixture unwritable: " +
                           path.string());
}

CanonicalValue &Member(CanonicalValue::Object &object, std::string_view name) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &field) { return field.first == name; });
  if (found == object.end())
    throw std::logic_error("catalog presence fixture missing member: " +
                           std::string(name));
  return found->second;
}

// Rewrites one provider manifest with a recomputed content revision, so only
// the mutation under test differs from a valid maintained record.
void RewriteProviderManifest(
    const fs::path &source, const fs::path &target,
    const std::function<void(CanonicalValue::Object &)> &mutate) {
  auto members =
      deployment::parse_strict_json_model_yaml(Read(source)).root().as_object();
  mutate(members);
  const auto *schema =
      deployment::build_qualified_closed_schema_registry()
          .find_catalog_schema_family(core::CatalogRecordKind::kProviderManifest);
  ASSERT_NE(schema, nullptr);
  ASSERT_NE(schema->compiled_schema(), nullptr);
  const auto normalized =
      deployment::validate_and_normalize_unqualified_artifact_candidate(
          *schema->compiled_schema(),
          deployment::parse_strict_json_model_yaml(
              deployment::canonical_json(CanonicalValue(std::move(members)))));
  const std::string revision = detail::compute_content_identity(
      normalized.normalized_root(),
      {detail::catalog_record_identity_domain("provider_manifest"),
       std::string(core::kProviderManifestApiVersion),
       "provider_manifest_revision"});
  auto rewritten = normalized.normalized_root().as_object();
  Member(rewritten, "provider_manifest_revision") = CanonicalValue(revision);
  Write(target, deployment::canonical_json(CanonicalValue(std::move(rewritten))));
}

detail::PreparedActivationMaterialization
MaterializeEmptyCell(const fs::path &root) {
  const CanonicalValue intent_value(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment_intent.v2")},
      {"deployment_artifact_id", CanonicalValue("catalog_presence_activation")},
      {"name", CanonicalValue("catalog_presence_activation")},
      {"attachments", CanonicalValue(CanonicalValue::Array{})},
      {"components", CanonicalValue(CanonicalValue::Array{})},
      {"policies", CanonicalValue(CanonicalValue::Object{})},
  });
  const auto intent =
      detail::parse_activation_deployment_intent(intent_value, {});
  return detail::materialize_prepared_activation(
      root, {}, intent, core::CellId("catalog_presence_cell"), {});
}

TEST(ActivationCatalogPresence, CellsWithComponentsHaveQualifiedExecutionInterruptProvidersAndEmptyCellsNone) {
  const CatalogCopy copy("execution_interrupt_provider");
  // The maintained Lite6 Scenario gives the cell components, so the normal
  // activation path adds the execution-core provider rows for it.
  const auto materialized =
      deployment::test::MaterializeMaintainedInterruptDeployment(
          copy.root(),
          deployment::parse_strict_json_model_yaml(
              R"({"interrupt_rules":[]})")
              .root());
  ASSERT_NE(materialized.bundle, nullptr);
  const detail::PreparedCatalogSource catalog(materialized.catalog_artifacts);
  for (const std::string_view id : {"execution.interrupt.resource_scoped",
                                    "execution.interrupt.contact_policy"}) {
    const auto *definition = core::find_builtin_capability_definition(id);
    ASSERT_NE(definition, nullptr);
    const core::ProgramCapabilityRequirementDTO requirement{
        core::CapabilityId(std::string(id)),
        core::CapabilityDefinitionRevision(std::string(definition->capability_definition_revision)),
        core::ProgramCapabilityDisposition::kRequired};
    EXPECT_TRUE(deployment::probe_program_cell_capability(
        requirement, *materialized.bundle, catalog)) << id;
    const auto &rows = materialized.bundle->value().capability_provider_bindings;
    EXPECT_EQ(std::count_if(rows.begin(), rows.end(), [&](const auto &row) {
      return row.capability_id.value() == id &&
             row.provider_manifest_id.value() == "manyforge.provider.execution_core" &&
             row.scope == core::CapabilityScope::kCellGlobal;
    }), 1);
  }

  // An empty cell is infrastructure-only: it carries no capability or
  // provider rows at all.
  const auto empty = MaterializeEmptyCell(copy.root());
  ASSERT_NE(empty.bundle, nullptr);
  EXPECT_TRUE(empty.bundle->value().capability_provider_bindings.empty());
}

constexpr std::string_view kRos2ControlManifest =
    "manyforge.provider.ros2_control.json";

TEST(ActivationCatalogPresence, AcceptsAnExtraCapabilityDefinitionSource) {
  const CatalogCopy copy("extra_capability");
  // A built-in target capability the maintained catalog does not carry yet.
  const auto extra = std::find_if(
      core::kBuiltinCapabilityDefinitionRegistry.begin(),
      core::kBuiltinCapabilityDefinitionRegistry.end(),
      [&copy](const auto &descriptor) {
        return descriptor.scope == core::CapabilityScope::kTargetBinding &&
               !descriptor.requirement_parameters_schema_ref.has_value() &&
               !fs::exists(copy.catalog("capability_definition") /
                           (std::string(descriptor.capability_id) + ".json"));
      });
  ASSERT_NE(extra, core::kBuiltinCapabilityDefinitionRegistry.end());
  Write(copy.catalog("capability_definition") /
            (std::string(extra->capability_id) + ".json"),
        deployment::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(core::kCapabilityDefinitionApiVersion))},
            {"capability_definition_revision",
             CanonicalValue(std::string(extra->capability_definition_revision))},
            {"capability_id", CanonicalValue(std::string(extra->capability_id))},
            {"provider_binding_kind",
             CanonicalValue(
                 std::string(core::to_string(extra->provider_binding_kind)))},
            {"scope", CanonicalValue(std::string(core::to_string(extra->scope)))},
        })));

  const auto materialized = MaterializeEmptyCell(copy.root());
  EXPECT_NE(materialized.bundle, nullptr);
}

TEST(ActivationCatalogPresence, AcceptsAnExtraProviderManifestAndExtraOffer) {
  const CatalogCopy copy("extra_offer");
  const auto manifests = copy.catalog("provider_manifest");
  // An extra (unselected) provider manifest record.
  RewriteProviderManifest(
      manifests / "manyforge.provider.io_signals.json",
      manifests / "manyforge.provider.presence_extra.json",
      [](CanonicalValue::Object &members) {
        Member(members, "provider_manifest_id") =
            CanonicalValue("manyforge.provider.presence_extra");
      });
  // An extra offer on a required lifecycle provider.
  RewriteProviderManifest(
      manifests / std::string(kRos2ControlManifest),
      manifests / std::string(kRos2ControlManifest),
      [](CanonicalValue::Object &members) {
        const auto *named = core::find_builtin_capability_definition(
            "planning.motion.named_target");
        ASSERT_NE(named, nullptr);
        auto offers = Member(members, "offered_capabilities").as_array();
        ASSERT_TRUE(std::none_of(offers.begin(), offers.end(), [](const auto &row) {
          return row.find_object_member("capability_id")->as_string() ==
                 "planning.motion.named_target";
        }));
        offers.emplace_back(CanonicalValue::Object{
            {"capability_definition_revision",
             CanonicalValue(std::string(named->capability_definition_revision))},
            {"capability_id", CanonicalValue("planning.motion.named_target")},
            {"required_endpoint_role_ids",
             CanonicalValue(CanonicalValue::Array{})},
        });
        std::sort(offers.begin(), offers.end(),
                  [](const auto &left, const auto &right) {
                    return left.find_object_member("capability_id")->as_string() <
                           right.find_object_member("capability_id")->as_string();
                  });
        Member(members, "offered_capabilities") =
            CanonicalValue(std::move(offers));
      });

  const auto materialized = MaterializeEmptyCell(copy.root());
  EXPECT_NE(materialized.bundle, nullptr);
}

TEST(ActivationCatalogPresence, RefusesALifecycleProviderWithoutItsStopContract) {
  const CatalogCopy copy("missing_stop_role");
  const auto manifest =
      copy.catalog("provider_manifest") / std::string(kRos2ControlManifest);
  RewriteProviderManifest(
      manifest, manifest, [](CanonicalValue::Object &members) {
        auto contracts = Member(members, "provider_contracts").as_array();
        const auto before = contracts.size();
        std::erase_if(contracts, [](const auto &row) {
          return row.find_object_member("provider_contract_ref")
                     ->find_object_member("provider_contract_role_id")
                     ->as_string() == "stop_contract";
        });
        ASSERT_EQ(contracts.size() + 1U, before);
        Member(members, "provider_contracts") =
            CanonicalValue(std::move(contracts));
      });

  try {
    (void)MaterializeEmptyCell(copy.root());
    FAIL() << "a lifecycle provider without its Stop contract was admitted";
  } catch (const std::logic_error &error) {
    EXPECT_NE(std::string(error.what())
                  .find("ROS control lifecycle provider manifest is not exact"),
              std::string::npos)
        << error.what();
  }
}

} // namespace
