#include <gtest/gtest.h>
#include <manyforge_deployment/scene_pose_source_policy_codec.hpp>

#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/contact_policy_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_contract_capability_declarations.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "composed_model_symbol_role_map_internal.hpp"
#include "content_identity_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "deployment_catalog_codec_internal.hpp"
#include "gripper_only_retention_fixture.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "lite6_structural_retention_fixture.hpp"
#include "execution_host_unit_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "planning_resource_map_internal.hpp"
#include "retained_bundle_internal.hpp"
#include "retention_store_fixtures.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace manyforge::deployment::detail {

/// Uninstalled test-only access to the deliberately producer-less context.
///
/// These values prove projection/materialization mechanics only. They are not
/// a production node-environment or compiler-build qualification, and no
/// equivalent callable factory is linked into the library.
class RetentionMaterializerTestAccess final {
public:
  [[nodiscard]] static BuildQualifiedRetentionContext make_context() {
    return BuildQualifiedRetentionContext(
        core::RetainedNodeEnvironmentDTO{
            "manyforge_retained_materializer_test",
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

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

struct LogicalSceneRuntimeCoverage final {
  std::vector<core::CapabilityProviderBindingDTO> bindings;
  std::vector<LogicalCapabilityTemplate> capabilities;
  std::vector<LogicalProviderEndpointTemplate> endpoints;
};

[[nodiscard]] LogicalSceneRuntimeCoverage
make_logical_scene_runtime_coverage(const core::RuntimeUnitDTO& runtime,
                                    const core::CellId& cell_id) {
  constexpr std::string_view kLogicalSceneEndpointRole =
      "logical_scene_services";
  const core::ProviderInstanceId provider_instance("logical_scene_provider");
  const core::ProviderManifestId provider_manifest(
      "manyforge.provider.logical_scene");
  const core::ProviderManifestRevision provider_manifest_revision{
      std::string(kZeroDigest)};
  LogicalSceneRuntimeCoverage result;
  const auto& declarations =
      runtime_contract_capabilities::kLogicalSceneServices;
  result.bindings.reserve(declarations.size());
  result.capabilities.reserve(declarations.size());
  result.endpoints.reserve(declarations.size());
  for (const auto capability_id : declarations) {
    const auto* descriptor =
        core::find_builtin_capability_definition(capability_id);
    if (descriptor == nullptr) {
      throw std::logic_error(
          "logical-scene regression fixture has a stale capability");
    }
    const auto capability_revision =
        descriptor->capability_definition_revision;
    const core::CapabilityProviderBindingId binding_id(
        "logical_scene_" + std::string(capability_id));
    const core::CapabilityProviderBindingRevision binding_revision{
        std::string(kZeroDigest)};
    const core::CapabilityId capability{std::string(capability_id)};
    const core::CapabilityDefinitionRevision revision{
        std::string(capability_revision)};
    const core::EndpointRoleId endpoint_role{
        std::string(kLogicalSceneEndpointRole)};
    result.bindings.push_back({
        binding_id,
        binding_revision,
        capability,
        revision,
        descriptor->scope,
        core::CellGlobalCapabilityProviderTargetDTO{cell_id},
        provider_instance,
        provider_manifest,
        provider_manifest_revision,
        {{endpoint_role,
          core::CapabilityEndpointSourceKind::kRuntimeUnit,
          runtime.runtime_unit_id,
          endpoint_role}},
    });
    result.capabilities.push_back({
        CapabilityProviderCapabilityOwner{binding_id, binding_revision},
        capability,
        revision,
        descriptor->scope,
        descriptor->provider_binding_kind,
        provider_instance,
        provider_manifest,
        provider_manifest_revision,
        std::string(capability_id),
    });
    result.endpoints.push_back({
        binding_id,
        binding_revision,
        endpoint_role,
        provider_instance,
        provider_manifest,
        provider_manifest_revision,
        RuntimeEndpointSourceTemplate{
            runtime.runtime_unit_id,
            runtime.runtime_profile_id,
            runtime.runtime_profile_revision,
            endpoint_role,
        },
        std::string(capability_id),
    });
  }
  return result;
}

static_assert(!std::is_aggregate_v<detail::BuildQualifiedRetentionContext>);
static_assert(
    !std::is_default_constructible_v<detail::BuildQualifiedRetentionContext>);
static_assert(
    !std::is_constructible_v<detail::BuildQualifiedRetentionContext,
                             core::RetainedNodeEnvironmentDTO,
                             core::ContentDigest, core::ContentDigest>);

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name) {
  const CanonicalValue *found = value.find_object_member(name);
  if (found == nullptr) {
    throw std::logic_error("test fixture is missing member " +
                           std::string(name));
  }
  return *found;
}

[[nodiscard]] const CanonicalValue::Array &
array_member(const CanonicalValue &value, std::string_view name) {
  return std::get<CanonicalValue::Array>(member(value, name).storage());
}

[[nodiscard]] const std::string &string_member(const CanonicalValue &value,
                                               std::string_view name) {
  return std::get<std::string>(member(value, name).storage());
}

[[nodiscard]] std::string replace_once(std::string value, std::string_view from,
                                       std::string_view to) {
  const std::size_t position = value.find(from);
  if (position == std::string::npos ||
      value.find(from, position + from.size()) != std::string::npos) {
    throw std::logic_error(
        "test replacement requires exactly one source occurrence");
  }
  value.replace(position, from.size(), to);
  return value;
}

[[nodiscard]] std::string empty_graph() {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] std::string deployment_document(std::string_view cell_id) {
  return "{\"api_version\":\"manyforge.deployment.v1\","
         "\"deployment_artifact_id\":\"materializer_canonical_empty\","
         "\"deployment_revision\":\"" +
         std::string(kZeroDigest) + "\",\"cell_id\":\"" + std::string(cell_id) +
         "\",\"name\":\"Materializer canonical empty\","
         "\"catalogs\":{\"records\":[]},"
         "\"graph\":" +
         empty_graph() +
         ",\"desired_control_configurations\":[],"
         "\"capability_provider_bindings\":[]}";
}

[[nodiscard]] RetentionQualifiedDeployment qualify_empty(
    std::string_view cell_id,
    std::optional<double> takeover_budget_s = std::nullopt,
    std::optional<double> planning_budget_s = std::nullopt,
    const std::vector<core::ScenePoseSourcePolicyDTO> &pose_sources = {}) {
  auto root = parse_strict_json_model_yaml(deployment_document(cell_id))
                  .root()
                  .as_object();
  if (takeover_budget_s.has_value()) {
    root.emplace_back(
        "policies",
        CanonicalValue(CanonicalValue::Object{
            {"retained_policy_records",
             CanonicalValue(CanonicalValue::Array{})},
            {"stop_stage_parameters", CanonicalValue(CanonicalValue::Array{})},
            {"commissioned_stop_interfaces",
             CanonicalValue(CanonicalValue::Array{})},
            {"motion_takeover_preparation_budget_s",
             CanonicalValue(*takeover_budget_s)},
        }));
  }
  if (planning_budget_s) {
    auto *policies = [&]() -> CanonicalValue * {
      for (auto &[name, value] : root)
        if (name == "policies")
          return &value;
      return nullptr;
    }();
    if (!policies)
      throw std::invalid_argument("planning fixture requires preparation");
    auto fields = policies->as_object();
    fields.emplace_back("motion_takeover_planning_budget_s",
                        CanonicalValue(*planning_budget_s));
    *policies = CanonicalValue(std::move(fields));
  }
  if (!pose_sources.empty()) {
    auto policy = std::find_if(root.begin(), root.end(), [](const auto &row) {
      return row.first == "policies";
    });
    if (policy == root.end())
      throw std::invalid_argument(
          "pose policy fixture requires policy envelope");
    auto fields = policy->second.as_object();
    fields.emplace_back("scene_pose_sources",
                        EncodeScenePoseSources(pose_sources));
    policy->second = CanonicalValue(std::move(fields));
  }
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(
          canonical_json(CanonicalValue(std::move(root)))),
      std::span<const QualifiedRuntimeProfile>{});
  const auto composition = compose_cell_occurrences(
      deployment.graph(), std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto selected_overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources, nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {},
      {}, {}, {}, {}, selected_overlay);
  return qualify_deployment_for_retention(deployment,
                                          compile_cell_ir(applicability));
}

[[nodiscard]] std::shared_ptr<const RetainedBundle> materialize_empty(
    std::string_view cell_id, IRawArtifactStore &store,
    std::optional<double> takeover_budget_s = std::nullopt,
    std::optional<double> planning_budget_s = std::nullopt,
    const std::vector<core::ScenePoseSourcePolicyDTO> &pose_sources = {}) {
  const auto qualified = qualify_empty(cell_id, takeover_budget_s,
                                       planning_budget_s, pose_sources);
  const detail::RejectingCompilerInputArtifactSource input_source;
  detail::RejectingCatalogArtifactStore catalog_store;
  return detail::RetainedBundleMaterializer::materialize(
      qualified, detail::RetentionMaterializerTestAccess::make_context(),
      input_source, {}, catalog_store, store);
}

[[nodiscard]] CanonicalValue replace_object_member(const CanonicalValue &value,
                                                   std::string_view member_name,
                                                   CanonicalValue replacement);

struct ContactPolicyRetentionFixture final {
  RetentionQualifiedDeployment qualified;
  QualifiedContactPolicy policy;
  std::string policy_bytes;
};

[[nodiscard]] ContactPolicyRetentionFixture
make_contact_policy_retention_fixture() {
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
                             {"body", CanonicalValue(CanonicalValue::Object{
                                 {"component_id", CanonicalValue("robot")},
                                 {"collision_body_id", CanonicalValue("tool_collision")}})},
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
  policy = replace_object_member(
      policy, "version",
      CanonicalValue(detail::compute_content_identity(
          policy,
          detail::ContentIdentitySpec{
              .identity_domain = "catalog-record-revision/contact_policy",
              .api_version = "manyforge.contact_policy.v1",
              .own_field = "version",
          })));
  const std::string policy_bytes = canonical_json(policy);
  const std::string policy_digest = sha256_digest(policy_bytes);
  const core::ContactPolicyCatalogRecordRefDTO reference{
      core::ContactPolicyId("selected_object_grasp"),
      core::ContactPolicyRevision(string_member(policy, "version")),
      core::SchemaId("manyforge.contact_policy.v1"),
      core::SchemaRevision(
          "sha256:"
          "f7adb173ef71d9de9011c85a19224cace5e721918fb1659fcada6ab93901ad33"),
      core::ContentDigest(policy_digest),
      core::ContentAddressedStoreKey("sha256/" + policy_digest.substr(7U)),
  };
  QualifiedContactPolicy qualified_policy = resolve_and_qualify_contact_policy(
      reference, reference.content_addressed_store_key, policy_bytes);

  const CanonicalValue catalogs = detail::encode_deployment_catalogs(
      std::array<core::CatalogRecordRefDTO, 1U>{reference});
  const CanonicalValue &records = member(catalogs, "records");
  const CanonicalValue deployment_root(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id", CanonicalValue("contact_policy_retention")},
      {"deployment_revision", CanonicalValue(std::string(kZeroDigest))},
      {"cell_id", CanonicalValue("contact_policy_cell")},
      {"name", CanonicalValue("Contact policy retention")},
      {"catalogs", catalogs},
      {"graph", parse_strict_json_model_yaml(empty_graph()).root()},
      {"desired_control_configurations",
       CanonicalValue(CanonicalValue::Array{})},
      {"capability_provider_bindings", CanonicalValue(CanonicalValue::Array{})},
      {"policies",
       CanonicalValue(CanonicalValue::Object{
           {"retained_policy_records", records},
           {"stop_stage_parameters", CanonicalValue(CanonicalValue::Array{})},
           {"commissioned_stop_interfaces",
            CanonicalValue(CanonicalValue::Array{})},
       })},
  });
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(deployment_root)), {});
  const auto composition = compose_cell_occurrences(
      deployment.graph(), std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources, nullptr);
  const std::array policies{qualified_policy};
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {},
      {}, {}, {}, {}, overlay, policies);
  return {qualify_deployment_for_retention(deployment,
                                           compile_cell_ir(applicability)),
          std::move(qualified_policy), policy_bytes};
}

class PolicyCompilerInputSource final
    : public detail::ICompilerInputArtifactSource {
public:
  template <typename Reference>
  PolicyCompilerInputSource(const Reference &reference, std::string bytes)
      : key_(reference.content_addressed_store_key),
        digest_(reference.content_digest), bytes_(std::move(bytes)) {}

  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &key,
               const core::ContentDigest &expected_digest,
               std::optional<std::uint64_t> expected_size) const override {
    if (key != key_ || expected_digest != digest_ ||
        (expected_size.has_value() && *expected_size != bytes_.size()) ||
        sha256_digest(bytes_) != expected_digest.value()) {
      return std::nullopt;
    }
    return bytes_;
  }

private:
  core::ContentAddressedStoreKey key_;
  core::ContentDigest digest_;
  std::string bytes_;
};

[[nodiscard]] std::string
require_product_artifact_bytes(const core::RetainedProductDTO &product,
                               std::string_view role,
                               const IRawArtifactStore &store) {
  const auto &artifacts = product.artifact_refs;
  const auto found =
      std::find_if(artifacts.begin(), artifacts.end(),
                   [role](const core::RetainedArtifactRefDTO &artifact) {
                     return artifact.artifact_role_id.value() == role;
                   });
  if (found == artifacts.end()) {
    throw std::logic_error("test bundle is missing retained product artifact");
  }
  const auto bytes =
      store.get_verified(found->content_addressed_store_key,
                         found->content_digest, found->byte_size);
  if (!bytes.has_value()) {
    throw std::logic_error(
        "test bundle artifact did not verify in the raw store");
  }
  return *bytes;
}

[[nodiscard]] std::string
require_product_artifact_bytes(const RetainedBundle &bundle,
                               std::string_view role,
                               const IRawArtifactStore &store) {
  return require_product_artifact_bytes(
      bundle.value().compiled_products.planning_collision_model, role, store);
}

[[nodiscard]] CanonicalValue replace_root_member(const CanonicalValue &root,
                                                 std::string_view member_name,
                                                 CanonicalValue replacement) {
  CanonicalValue::Object object = root.as_object();
  const auto found = std::find_if(object.begin(), object.end(),
                                  [member_name](const auto &member) {
                                    return member.first == member_name;
                                  });
  if (found == object.end()) {
    throw std::logic_error("test fixture is missing root member " +
                           std::string(member_name));
  }
  found->second = std::move(replacement);
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue replace_object_member(const CanonicalValue &value,
                                                   std::string_view member_name,
                                                   CanonicalValue replacement) {
  CanonicalValue::Object object = value.as_object();
  const auto found = std::find_if(object.begin(), object.end(),
                                  [member_name](const auto &member) {
                                    return member.first == member_name;
                                  });
  if (found == object.end()) {
    throw std::logic_error("test fixture is missing object member " +
                           std::string(member_name));
  }
  found->second = std::move(replacement);
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue
recompute_controller_configuration_digest(CanonicalValue controller) {
  const std::string digest = detail::compute_content_identity(
      controller,
      detail::ContentIdentitySpec{
          .identity_domain = "control-controller-configuration-digest",
          .api_version = std::string(detail::kControlDomainControlApiVersion) +
                         "#controller",
          .own_field = "configuration_digest",
          .root_shape = detail::ContentIdentityRootShape::kNestedRecord,
      });
  return replace_object_member(controller, "configuration_digest",
                               CanonicalValue(digest));
}

[[nodiscard]] CanonicalValue
recompute_control_source_conformance(CanonicalValue control) {
  CanonicalValue preimage(CanonicalValue::Object{
      {"api_version",
       CanonicalValue("manyforge.control_source_conformance.v1")},
      {"source_conformance_digest", CanonicalValue(std::string(kZeroDigest))},
      {"bindings", member(control, "bindings")},
      {"controllers", member(control, "controllers")},
  });
  const std::string digest = detail::compute_content_identity(
      preimage, detail::ContentIdentitySpec{
                    .identity_domain = "control-source-conformance-digest",
                    .api_version = "manyforge.control_source_conformance.v1",
                    .own_field = "source_conformance_digest",
                });
  CanonicalValue evidence(CanonicalValue::Object{
      {"api_version",
       CanonicalValue("manyforge.control_source_conformance.v1")},
      {"source_conformance_digest", CanonicalValue(digest)},
  });
  return replace_object_member(control, "source_conformance",
                               std::move(evidence));
}

[[nodiscard]] CanonicalValue replace_first_array_object_member(
    const CanonicalValue &root, std::string_view array_name,
    std::string_view member_name, CanonicalValue replacement) {
  CanonicalValue::Array rows = array_member(root, array_name);
  if (rows.empty()) {
    throw std::logic_error("test fixture array is empty: " +
                           std::string(array_name));
  }
  rows.front() =
      replace_object_member(rows.front(), member_name, std::move(replacement));
  return replace_root_member(root, array_name, CanonicalValue(std::move(rows)));
}

[[nodiscard]] CanonicalValue reidentify_retained_bundle(CanonicalValue root) {
  root = replace_root_member(root, "retained_bundle_id",
                             CanonicalValue(std::string(kZeroDigest)));
  const std::string identity = detail::compute_content_identity(
      root, detail::ContentIdentitySpec{
                .identity_domain = "retained-bundle-id",
                .api_version = std::string(core::kRetainedBundleApiVersion),
                .own_field = "retained_bundle_id",
            });
  return replace_root_member(root, "retained_bundle_id",
                             CanonicalValue(identity));
}

class CountingRawArtifactStore final : public IRawArtifactStore {
public:
  [[nodiscard]] StoredRawArtifact
  put_if_absent(std::string_view bytes) override {
    ++put_calls_;
    return {
        core::ContentDigest(sha256_digest(bytes)),
        core::ContentAddressedStoreKey("sha256/" +
                                       sha256_digest(bytes).substr(7U)),
        static_cast<std::uint64_t>(bytes.size()),
        true,
    };
  }

  [[nodiscard]] std::optional<std::string>
  get_verified(const core::ContentAddressedStoreKey &,
               const core::ContentDigest &, std::uint64_t) const override {
    ++get_calls_;
    return std::nullopt;
  }

  [[nodiscard]] std::size_t put_calls() const noexcept { return put_calls_; }
  [[nodiscard]] std::size_t get_calls() const noexcept { return get_calls_; }

private:
  std::size_t put_calls_{0U};
  mutable std::size_t get_calls_{0U};
};

TEST(RetainedBundleMaterializer,
     CanonicalEmptyUsesOrdinaryCompilerUrdfSrdfAndClosedEnvelope) {
  const auto qualified = qualify_empty("empty_cell");
  const auto &composition =
      qualified.compiler_ir().applicability().composition();
  const std::string expected_urdf =
      emit_canonical_urdf_document(composition.flattened_urdf());
  const std::string expected_srdf =
      emit_canonical_srdf_document(composition.flattened_srdf());
  ASSERT_EQ(composition.flattened_urdf().links.size(), 1U);
  EXPECT_EQ(composition.flattened_urdf().links.front().name,
            composition.cell_frame().compiled_frame_name);
  EXPECT_NE(expected_urdf.find("<link"), std::string::npos);
  EXPECT_EQ(expected_srdf.find("<group"), std::string::npos);

  test::InMemoryRawArtifactStoreFixture store;
  const detail::RejectingCompilerInputArtifactSource input_source;
  detail::RejectingCatalogArtifactStore catalog_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      qualified, detail::RetentionMaterializerTestAccess::make_context(),
      input_source, {}, catalog_store, store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  EXPECT_EQ(bundle->canonical_bytes(),
            canonical_json(bundle->normalized_root()));
  EXPECT_EQ(store.size(), 3U);

  const CanonicalValue &root = bundle->normalized_root();
  EXPECT_EQ(root.as_object().size(), 12U);
  EXPECT_EQ(string_member(root, "api_version"),
            core::kRetainedBundleApiVersion);
  EXPECT_EQ(string_member(root, "deployment_artifact_id"),
            "materializer_canonical_empty");
  EXPECT_EQ(string_member(root, "retained_bundle_id"),
            bundle->retained_bundle_id().value());
  EXPECT_EQ(bundle->value().api_version, core::kRetainedBundleApiVersion);
  EXPECT_EQ(bundle->value().retained_bundle_id, bundle->retained_bundle_id());
  EXPECT_EQ(bundle->value().cell_graph,
            qualified.compiler_ir().applicability().composition().graph());
  EXPECT_TRUE(bundle->value().catalogs.records.empty());
  EXPECT_TRUE(bundle->value().catalogs.schema_artifacts.empty());
  ASSERT_TRUE(bundle->value().compiled_products.viewer_model.has_value());
  EXPECT_TRUE(bundle->value().compiled_products.symbol_role_maps.empty());
  EXPECT_EQ(bundle->value().provenance.output_records.size(), 2U);

  const CanonicalValue &catalogs = member(root, "catalogs");
  EXPECT_TRUE(array_member(catalogs, "records").empty());
  EXPECT_TRUE(array_member(catalogs, "schema_artifacts").empty());
  EXPECT_TRUE(array_member(root, "desired_control_configurations").empty());
  EXPECT_TRUE(array_member(root, "capability_provider_bindings").empty());
  EXPECT_TRUE(array_member(root, "retained_policy_records").empty());
  EXPECT_TRUE(array_member(root, "runtime_contract_requirements").empty());
  EXPECT_TRUE(bundle->value().retained_policy_records.empty());

  const CanonicalValue &products = member(root, "compiled_products");
  const CanonicalValue &planning = member(products, "planning_collision_model");
  const auto &artifacts = array_member(planning, "artifact_refs");
  ASSERT_EQ(artifacts.size(), 3U);
  EXPECT_EQ(string_member(artifacts[0], "artifact_role_id"),
            detail::kPlanningResourceMapArtifactRole);
  EXPECT_EQ(string_member(artifacts[1], "artifact_role_id"), "srdf");
  EXPECT_EQ(string_member(artifacts[2], "artifact_role_id"), "urdf");

  const std::string expected_resource_map =
      detail::emit_planning_resource_map({});
  const auto &viewer = *bundle->value().compiled_products.viewer_model;
  EXPECT_EQ(viewer.product_id.value(), "cell_viewer_model");
  EXPECT_EQ(viewer.product_kind, core::RetainedProductKind::kViewerModel);
  EXPECT_EQ(viewer.product_contract_id.value(), "manyforge.viewer_model");
  ASSERT_EQ(viewer.artifact_refs.size(), 2U);
  EXPECT_EQ(viewer.artifact_refs[0].artifact_role_id.value(),
            detail::kPlanningResourceMapArtifactRole);
  EXPECT_EQ(viewer.artifact_refs[1].artifact_role_id.value(), "urdf");
  EXPECT_EQ(require_product_artifact_bytes(
                viewer, detail::kPlanningResourceMapArtifactRole, store),
            expected_resource_map);
  EXPECT_EQ(require_product_artifact_bytes(viewer, "urdf", store),
            expected_urdf);

  const std::string resource_map_digest = sha256_digest(expected_resource_map);
  const std::string srdf_digest = sha256_digest(expected_srdf);
  const std::string urdf_digest = sha256_digest(expected_urdf);
  EXPECT_EQ(store.get_verified(core::ContentAddressedStoreKey(
                                   "sha256/" + resource_map_digest.substr(7U)),
                               core::ContentDigest(resource_map_digest),
                               expected_resource_map.size()),
            expected_resource_map);
  EXPECT_EQ(
      store.get_verified(
          core::ContentAddressedStoreKey("sha256/" + srdf_digest.substr(7U)),
          core::ContentDigest(srdf_digest), expected_srdf.size()),
      expected_srdf);
  EXPECT_EQ(
      store.get_verified(
          core::ContentAddressedStoreKey("sha256/" + urdf_digest.substr(7U)),
          core::ContentDigest(urdf_digest), expected_urdf.size()),
      expected_urdf);

  const CanonicalValue &provenance = member(root, "provenance");
  EXPECT_TRUE(array_member(provenance, "source_records").empty());
  EXPECT_TRUE(array_member(provenance, "generator_records").empty());
  EXPECT_EQ(array_member(provenance, "output_records").size(), 2U);
}

TEST(RetainedBundleMaterializer,
     ContactPolicySelectionIsRetainedAndReloadedAsOneExactCatalogReferent) {
  auto fixture = make_contact_policy_retention_fixture();
  PolicyCompilerInputSource input_source(fixture.policy.reference(),
                                         fixture.policy_bytes);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source, {},
      catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  ASSERT_EQ(bundle->value().retained_policy_records.size(), 1U);
  EXPECT_EQ(bundle->value().retained_policy_records.front(),
            core::CatalogRecordRefDTO(fixture.policy.reference()));
  ASSERT_EQ(bundle->value().catalogs.records.size(), 1U);
  EXPECT_EQ(bundle->value().catalogs.records.front(),
            bundle->value().retained_policy_records.front());
  EXPECT_EQ(catalog_store.size(), 1U);
  EXPECT_NO_THROW(
      static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
          bundle->canonical_bytes())));

  CanonicalValue changed =
      replace_root_member(bundle->normalized_root(), "retained_policy_records",
                          CanonicalValue(CanonicalValue::Array{}));
  changed = reidentify_retained_bundle(std::move(changed));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(changed))),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     RehashedViewerCannotDivergeFromAuthoritativePlanningUrdf) {
  test::InMemoryRawArtifactStoreFixture store;
  const auto bundle = materialize_empty("viewer_splice_cell", store);
  const auto &products = bundle->value().compiled_products;
  ASSERT_TRUE(products.viewer_model.has_value());
  ASSERT_EQ(products.planning_collision_model.artifact_refs.size(), 3U);

  core::RetainedProductDTO changed_viewer = *products.viewer_model;
  changed_viewer.artifact_refs[1] =
      products.planning_collision_model.artifact_refs[1];
  changed_viewer.artifact_refs[1].artifact_role_id =
      core::ArtifactRoleId("urdf");
  ASSERT_NO_THROW(
      detail::require_current_retained_product_conformance(changed_viewer));

  CanonicalValue root = bundle->normalized_root();
  CanonicalValue compiled_products = member(root, "compiled_products");
  compiled_products = replace_object_member(
      compiled_products, "viewer_model",
      detail::retained_product_projection(changed_viewer));
  root = replace_root_member(root, "compiled_products",
                             std::move(compiled_products));

  CanonicalValue provenance = member(root, "provenance");
  CanonicalValue::Array outputs = array_member(provenance, "output_records");
  const auto viewer_output = std::find_if(
      outputs.begin(), outputs.end(), [](const CanonicalValue &output) {
        return string_member(output, "product_id") == "cell_viewer_model";
      });
  ASSERT_NE(viewer_output, outputs.end());
  *viewer_output = replace_object_member(
      *viewer_output, "content_digest",
      CanonicalValue(
          detail::retained_product_output_digest(changed_viewer).value()));
  provenance = replace_object_member(provenance, "output_records",
                                     CanonicalValue(std::move(outputs)));
  root = replace_root_member(root, "provenance", std::move(provenance));
  root = reidentify_retained_bundle(std::move(root));

  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(root))),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     MissingReferenceRuntimeProfileRejectsBeforeAnyStoreEffect) {
  const auto fixture = test::make_runtime_only_retention_fixture();
  ASSERT_EQ(fixture.qualified.deployment().graph().graph().runtime_units.size(),
            1U);
  ASSERT_EQ(fixture.qualified.compiler_ir().catalog_record_refs().size(), 1U);

  CountingRawArtifactStore store;
  const detail::RejectingCompilerInputArtifactSource input_source;
  detail::RejectingCatalogArtifactStore catalog_store;
  EXPECT_THROW(
      static_cast<void>(detail::RetainedBundleMaterializer::materialize(
          fixture.qualified,
          detail::RetentionMaterializerTestAccess::make_context(), input_source,
          {}, catalog_store, store)),
      RetainedBundleError);
  EXPECT_EQ(store.put_calls(), 0U);
  EXPECT_EQ(store.get_calls(), 0U);
}

TEST(RetainedBundleMaterializer,
     UnqualifiedRuntimeRowRejectsBeforeAnyStoreEffect) {
  const auto fixture = test::make_runtime_only_retention_fixture();
  auto deployment = test::produce_runtime_only_deployment(
      fixture.runtime_profile,
      test::runtime_only_graph(fixture.runtime_profile, "unqualified_runtime"));
  auto compiler = test::compile_runtime_only_deployment(
      deployment, fixture.runtime_profile);
  auto qualified = qualify_deployment_for_retention(deployment, compiler);
  const test::RuntimeOnlyCompilerInputArtifactSource input_source(
      fixture.runtime_profile);
  CountingRawArtifactStore store;
  detail::RejectingCatalogArtifactStore catalog_store;

  EXPECT_THROW(
      static_cast<void>(detail::RetainedBundleMaterializer::materialize(
          qualified, detail::RetentionMaterializerTestAccess::make_context(),
          input_source, {}, catalog_store, store)),
      RetainedBundleError);
  EXPECT_EQ(store.put_calls(), 0U);
  EXPECT_EQ(store.get_calls(), 0U);
}

TEST(RetainedBundleMaterializer,
     ReferenceRuntimeProductIsExactDeterministicAndClosed) {
  const auto fixture = test::make_runtime_only_retention_fixture();
  const test::RuntimeOnlyCompilerInputArtifactSource input_source(
      fixture.runtime_profile);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source, {},
      catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  ASSERT_EQ(bundle->value().compiled_products.runtime_unit_products.size(), 1U);
  const auto &group =
      bundle->value().compiled_products.runtime_unit_products.front();
  EXPECT_EQ(group.runtime_unit_id.value(), detail::kReferenceRuntimeUnitId);
  ASSERT_EQ(group.products.size(), 1U);
  const auto &product = group.products.front();
  EXPECT_EQ(product.product_kind,
            core::RetainedProductKind::kRuntimeUnitConfiguration);
  EXPECT_EQ(product.product_contract_id.value(),
            detail::kReferenceRuntimeProductContractId);
  EXPECT_EQ(product.product_contract_revision,
            detail::reference_runtime_product_contract_revision());
  ASSERT_EQ(product.artifact_refs.size(), 2U);

  const std::string manifest = require_product_artifact_bytes(
      product, detail::kReferenceRuntimeManifestArtifactRole, raw_store);
  const std::string launchable = require_product_artifact_bytes(
      product, detail::kReferenceRuntimeLaunchableArtifactRole, raw_store);
  EXPECT_EQ(launchable, detail::reference_runtime_launchable_script());
  EXPECT_NO_THROW(static_cast<void>(
      detail::parse_and_verify_reference_runtime_manifest(manifest)));
  EXPECT_NO_THROW(
      detail::require_reference_runtime_product_matches_retained_bundle(
          manifest, launchable, bundle->value(), group.runtime_unit_id));
  EXPECT_EQ(catalog_store.size(), 1U);

  test::InMemoryCatalogArtifactStoreFixture second_catalog_store;
  test::InMemoryRawArtifactStoreFixture second_raw_store;
  const auto regenerated = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source, {},
      second_catalog_store, second_raw_store);
  ASSERT_NE(regenerated, nullptr);
  EXPECT_EQ(regenerated->retained_bundle_id(), bundle->retained_bundle_id());
  EXPECT_EQ(regenerated->canonical_bytes(), bundle->canonical_bytes());

  const std::string changed_manifest =
      replace_once(manifest, "posix.1-2017", "posix.1-2018");
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_reference_runtime_manifest(
          changed_manifest)),
      RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     ReferenceRuntimeManifestRejectsEveryMutableExecutionSeam) {
  const auto fixture = test::make_runtime_only_retention_fixture();
  const test::RuntimeOnlyCompilerInputArtifactSource input_source(
      fixture.runtime_profile);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source, {},
      catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  const auto &group =
      bundle->value().compiled_products.runtime_unit_products.front();
  const auto &product = group.products.front();
  const std::string manifest = require_product_artifact_bytes(
      product, detail::kReferenceRuntimeManifestArtifactRole, raw_store);
  const std::string launchable = require_product_artifact_bytes(
      product, detail::kReferenceRuntimeLaunchableArtifactRole, raw_store);

  struct Mutation final {
    std::string_view name;
    std::string from;
    std::string to;
  };
  const std::array<Mutation, 9U> mutations{{
      {"artifact role", "launchable_script", "launchable_binary"},
      {"artifact media", "application/x-sh", "application/octet-stream"},
      {"artifact digest",
       sha256_digest(detail::reference_runtime_launchable_script()),
       std::string(kZeroDigest)},
      {"retriever", "manyforge.retained_raw_artifact",
       "manyforge.mutable_local_artifact"},
      {"adapter", "manyforge.posix_sh.retained_script",
       "manyforge.arbitrary_command"},
      {"lifecycle", "manager_managed", "externally_managed"},
      {"runtime role", "manager_fixture", "arbitrary_runtime_role"},
      {"endpoint role", "runtime_status", "arbitrary_status"},
      {"endpoint contract", "manyforge.reference_runtime.status_stdout",
       "manyforge.reference_runtime.status_socket"},
  }};
  for (const auto &mutation : mutations) {
    SCOPED_TRACE(mutation.name);
    const std::string changed =
        replace_once(manifest, mutation.from, mutation.to);
    EXPECT_THROW(
        static_cast<void>(
            detail::parse_and_verify_reference_runtime_manifest(changed)),
        RetainedBundleError);
  }

  CanonicalValue::Object injected =
      parse_strict_json_model_yaml(manifest).root().as_object();
  injected.emplace_back("command", CanonicalValue(CanonicalValue::Array{
                                       CanonicalValue("/bin/other")}));
  injected.emplace_back("environment",
                        CanonicalValue(CanonicalValue::Object{
                            {"UNQUALIFIED", CanonicalValue("injected")},
                        }));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_reference_runtime_manifest(
          canonical_json(CanonicalValue(std::move(injected))))),
      RetainedBundleError);

  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_reference_runtime_manifest(
          std::string(detail::kMaxReferenceRuntimeManifestBytes + 1U, 'x'))),
      RetainedBundleError);
  EXPECT_THROW(
      detail::require_reference_runtime_product_matches_retained_bundle(
          manifest,
          std::string(detail::kMaxReferenceRuntimeLaunchableBytes + 1U, 'x'),
          bundle->value(), group.runtime_unit_id),
      RetainedBundleError);
  EXPECT_NO_THROW(
      detail::require_reference_runtime_product_matches_retained_bundle(
          manifest, launchable, bundle->value(), group.runtime_unit_id));
}

TEST(RetainedBundleMaterializer,
     PassiveLite6RetainsOpaqueCatalogAndExactStructuralClosure) {
  const auto fixture = test::make_lite6_structural_retention_fixture();
  const auto qualified = test::qualify_lite6_passive_deployment(fixture);
  const test::Lite6CompilerInputArtifactSource input_source(fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      qualified, detail::RetentionMaterializerTestAccess::make_context(),
      input_source, fixture.structural_preparations(), catalog_store,
      raw_store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  ASSERT_EQ(bundle->value().catalogs.records.size(), 1U);
  EXPECT_EQ(bundle->value().catalogs.records.front(),
            core::CatalogRecordRefDTO(fixture.catalog_reference));
  ASSERT_EQ(bundle->value().provenance.source_records.size(), 1U);
  ASSERT_EQ(bundle->value().provenance.generator_records.size(), 1U);
  EXPECT_EQ(std::get<core::RetainedModelAssetSourceRecordDTO>(
                bundle->value().provenance.source_records.front())
                .record_id,
            fixture.catalog_reference.record_id);
  EXPECT_EQ(bundle->value().provenance.generator_records.front(),
            fixture.preparation.generator_record());
  EXPECT_EQ(catalog_store.size(), 1U);
  EXPECT_EQ(catalog_store.get_verified(
                fixture.catalog_reference.content_addressed_store_key,
                fixture.catalog_reference.content_digest,
                fixture.catalog_bytes.size()),
            fixture.catalog_bytes);
  EXPECT_NE(fixture.catalog_reference.content_addressed_store_key.value(),
            detail::retained_store_key_for_digest(
                fixture.catalog_reference.content_digest.value()));

  for (const auto &artifact : fixture.source_artifacts) {
    EXPECT_EQ(raw_store.get_verified(
                  artifact.store_key, artifact.content_digest,
                  static_cast<std::uint64_t>(artifact.bytes.size())),
              artifact.bytes)
        << artifact.logical_uri;
  }
  ASSERT_EQ(
      bundle->value()
          .compiled_products.planning_collision_model.artifact_refs.size(),
      3U);
  for (const auto &artifact :
       bundle->value()
           .compiled_products.planning_collision_model.artifact_refs) {
    EXPECT_TRUE(raw_store
                    .get_verified(artifact.content_addressed_store_key,
                                  artifact.content_digest, artifact.byte_size)
                    .has_value());
  }

  ASSERT_TRUE(bundle->value().compiled_products.viewer_model.has_value());
  const auto &viewer = *bundle->value().compiled_products.viewer_model;
  ASSERT_EQ(viewer.artifact_refs.size(), 2U);
  EXPECT_EQ(require_product_artifact_bytes(viewer, "urdf", raw_store),
            require_product_artifact_bytes(
                bundle->value().compiled_products.planning_collision_model,
                "urdf", raw_store));
  EXPECT_EQ(require_product_artifact_bytes(
                viewer, detail::kPlanningResourceMapArtifactRole, raw_store),
            require_product_artifact_bytes(
                bundle->value().compiled_products.planning_collision_model,
                detail::kPlanningResourceMapArtifactRole, raw_store));

  ASSERT_EQ(bundle->value().compiled_products.symbol_role_maps.size(), 1U);
  const auto &symbol_product =
      bundle->value().compiled_products.symbol_role_maps.front();
  EXPECT_EQ(symbol_product.product_id.value(),
            "cell_composed_model_symbol_role_map");
  EXPECT_EQ(symbol_product.product_kind,
            core::RetainedProductKind::kSymbolRoleMap);
  EXPECT_EQ(symbol_product.product_contract_id.value(),
            detail::kComposedModelSymbolRoleMapContractId);
  ASSERT_EQ(symbol_product.artifact_refs.size(), 1U);
  EXPECT_EQ(symbol_product.artifact_refs.front().artifact_role_id.value(),
            detail::kComposedModelSymbolRoleMapArtifactRole);
  EXPECT_EQ(symbol_product.artifact_refs.front().media_type,
            detail::kComposedModelSymbolRoleMapMediaType);

  const std::string symbol_map = require_product_artifact_bytes(
      symbol_product, detail::kComposedModelSymbolRoleMapArtifactRole,
      raw_store);
  const CanonicalValue parsed_symbol_map =
      detail::parse_and_verify_composed_model_symbol_role_map(symbol_map);
  EXPECT_EQ(canonical_json(parsed_symbol_map), symbol_map);
  EXPECT_NO_THROW(detail::require_composed_model_symbol_role_map_matches(
      symbol_map, qualified.compiler_ir()));
  const auto &composition =
      qualified.compiler_ir().applicability().composition();
  EXPECT_EQ(array_member(parsed_symbol_map, "model_occurrences").size(),
            composition.model_occurrences().size());
  EXPECT_EQ(array_member(parsed_symbol_map, "symbol_rows").size(),
            composition.symbol_rewrites().size());
  EXPECT_EQ(array_member(parsed_symbol_map, "reference_rows").size(),
            composition.reference_rewrites().size());
  EXPECT_EQ(array_member(parsed_symbol_map, "coordinate_rows").size(),
            composition.coordinates().size());
  EXPECT_EQ(array_member(parsed_symbol_map, "fixed_connections").size(),
            composition.fixed_connections().size());
  EXPECT_EQ(array_member(parsed_symbol_map, "frame_paths").size(),
            composition.frame_paths().size());
  EXPECT_EQ(
      array_member(parsed_symbol_map, "control_binding_coordinate_maps").size(),
      composition.control_binding_coordinate_maps().size());
  EXPECT_EQ(array_member(parsed_symbol_map, "target_role_maps").size(),
            composition.targets().size());

  std::set<std::string> source_symbols;
  std::set<std::string> compiled_symbols;
  for (const auto &row : array_member(parsed_symbol_map, "symbol_rows")) {
    const std::string source_key = string_member(row, "model_instance_id") +
                                   "\x1f" + string_member(row, "kind") +
                                   "\x1f" + string_member(row, "scope_name") +
                                   "\x1f" + string_member(row, "source_name");
    EXPECT_TRUE(source_symbols.insert(source_key).second);
    EXPECT_TRUE(
        compiled_symbols.insert(string_member(row, "compiled_name")).second);
  }
  EXPECT_EQ(source_symbols.size(), compiled_symbols.size());
}

TEST(RetainedBundleMaterializer,
     ControllableLite6RetainsExactTargetBindingAndCatalogClosure) {
  const auto fixture =
      test::make_lite6_ros2_control_observed_consumer_retention_fixture();
  // Retain observation-only controller assertions at the cold emit seam.
  const auto cold_fixture = test::make_lite6_controllable_retention_fixture();
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  const auto expected_catalog_count =
      fixture.qualified.compiler_ir().catalog_record_refs().size();
  EXPECT_EQ(bundle->value().catalogs.records.size(), expected_catalog_count);
  EXPECT_EQ(bundle->value().catalogs.records,
            fixture.qualified.compiler_ir().catalog_record_refs());
  // Catalog records may share one exact closed-schema descriptor.
  std::set<std::pair<std::string, std::string>> expected_schema_identities;
  std::map<std::pair<std::string, std::string>, std::string> expected_schema_bytes;
  for (const auto &record : fixture.qualified.compiler_ir().catalog_record_refs()) {
    std::visit([&](const auto &reference) {
      const auto identity = std::make_pair(reference.schema_id.value(),
                                           reference.schema_revision.value());
      expected_schema_identities.insert(identity);
      using Reference = std::remove_cvref_t<decltype(reference)>;
      const auto *schema = build_qualified_closed_schema_registry()
                               .find_qualified_catalog_schema(
                                   Reference::kRecordKind, reference.schema_id,
                                   reference.schema_revision);
      ASSERT_NE(schema, nullptr);
      expected_schema_bytes.emplace(identity, schema->descriptor_bytes());
    }, record);
  }
  EXPECT_EQ(bundle->value().catalogs.schema_artifacts.size(),
            expected_schema_identities.size());
  std::set<std::pair<std::string, std::string>> retained_schema_identities;
  for (const auto &schema : bundle->value().catalogs.schema_artifacts) {
    EXPECT_TRUE(retained_schema_identities
                    .emplace(schema.schema_id.value(),
                             schema.schema_revision.value())
                    .second);
    const auto expected_bytes = expected_schema_bytes.find(
        {schema.schema_id.value(), schema.schema_revision.value()});
    ASSERT_NE(expected_bytes, expected_schema_bytes.end());
    const auto retained_bytes = raw_store.get_verified(
        schema.content_addressed_store_key, schema.content_digest,
        static_cast<std::uint64_t>(expected_bytes->second.size()));
    ASSERT_TRUE(retained_bytes.has_value());
    EXPECT_EQ(*retained_bytes, expected_bytes->second);
  }
  EXPECT_EQ(retained_schema_identities, expected_schema_identities);
  EXPECT_EQ(bundle->value().provenance.source_records.size(),
            expected_catalog_count);
  EXPECT_EQ(bundle->value().provenance.generator_records.size(), 1U);
  EXPECT_EQ(catalog_store.size(), expected_catalog_count);
  ASSERT_EQ(bundle->value().desired_control_configurations.size(), 1U);
  EXPECT_EQ(bundle->value().capability_provider_bindings,
            fixture.qualified.deployment().applicability_inputs().value()
                .capability_provider_bindings);
  ASSERT_EQ(bundle->value().compiled_products.control_domain_products.size(),
            1U);
  const auto &control_group =
      bundle->value().compiled_products.control_domain_products.front();
  EXPECT_EQ(control_group.control_domain_id.value(), test::kLite6ArmDomainId);
  ASSERT_EQ(control_group.products.size(), 1U);
  EXPECT_EQ(control_group.products.front().product_kind,
            core::RetainedProductKind::kControlDomainControl);
  EXPECT_EQ(control_group.products.front().product_contract_id.value(),
            "manyforge.control_domain_control");
  ASSERT_EQ(control_group.products.front().artifact_refs.size(), 1U);
  EXPECT_EQ(control_group.products.front()
                .artifact_refs.front()
                .artifact_role_id.value(),
            "control_domain_control");
  const std::string control_bytes = require_product_artifact_bytes(
      control_group.products.front(), detail::kControlDomainControlArtifactRole,
      raw_store);
  const std::string cold_control_bytes = detail::emit_control_domain_control(
      cold_fixture.qualified.compiler_ir(), control_group.control_domain_id);
  const CanonicalValue control =
      detail::parse_and_verify_control_domain_control(cold_control_bytes);
  EXPECT_NO_THROW(detail::require_control_domain_control_matches(
      cold_control_bytes, cold_fixture.qualified.compiler_ir(),
      control_group.control_domain_id));
  EXPECT_NO_THROW(detail::require_control_domain_control_matches(
      control_bytes, fixture.qualified.compiler_ir(),
      core::ControlDomainId(std::string(test::kLite6ArmDomainId))));
  const auto chained_fixture =
      test::make_lite6_chained_controllable_retention_fixture();
  const std::string chained_bytes = detail::emit_control_domain_control(
      chained_fixture.qualified.compiler_ir(),
      core::ControlDomainId(std::string(test::kLite6ArmDomainId)));
  EXPECT_THROW(detail::require_control_domain_control_matches(
                   chained_bytes, cold_fixture.qualified.compiler_ir(),
                   core::ControlDomainId(std::string(test::kLite6ArmDomainId))),
               RetainedBundleError);
  EXPECT_THROW(
      detail::require_control_domain_control_matches_retained_bundle(
          chained_bytes, bundle->value(), control_group.control_domain_id),
      RetainedBundleError);
  ASSERT_EQ(array_member(control, "bindings").size(), 1U);
  EXPECT_EQ(
      array_member(array_member(control, "bindings").front(), "command_claims")
          .size(),
      6U);
  EXPECT_EQ(
      array_member(array_member(control, "bindings").front(), "state_claims")
          .size(),
      6U);
  const auto &not_stoppable = array_member(control, "stop_applicability");
  ASSERT_EQ(not_stoppable.size(), 1U);
  EXPECT_EQ(string_member(not_stoppable.front(), "disposition"),
            "not_stoppable");
  EXPECT_EQ(string_member(not_stoppable.front(), "control_route_id"),
            "arm_position");
  EXPECT_EQ(string_member(not_stoppable.front(), "stop_evidence_policy_id"),
            "lite6_not_stoppable");
  EXPECT_EQ(not_stoppable.front().find_object_member("containment_command"),
            nullptr);
  CanonicalValue::Array opened_stop_rows = not_stoppable;
  CanonicalValue::Object opened_not_stoppable =
      opened_stop_rows.front().as_object();
  opened_not_stoppable.emplace_back("containment_command",
                                    CanonicalValue(nullptr));
  opened_stop_rows.front() = CanonicalValue(std::move(opened_not_stoppable));
  const CanonicalValue opened_control =
      replace_object_member(control, "stop_applicability",
                            CanonicalValue(std::move(opened_stop_rows)));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_control_domain_control(
          canonical_json(opened_control))),
      RetainedBundleError);
  ASSERT_EQ(array_member(control, "controllers").size(), 1U);
  EXPECT_EQ(string_member(member(array_member(control, "controllers").front(),
                                 "implementation"),
                          "implementation_id"),
            "manyforge.ros2_control.joint_trajectory_controller.direct");
  ASSERT_EQ(bundle->value().compiled_products.runtime_unit_products.size(), 1U);
  EXPECT_EQ(bundle->value()
                .compiled_products.runtime_unit_products.front()
                .runtime_unit_id.value(),
            test::kLite6Ros2ControlExecutionRuntimeId);
  ASSERT_EQ(bundle->value()
                .compiled_products.runtime_unit_products.front()
                .products.size(),
            1U);
  ASSERT_TRUE(bundle->value().compiled_products.viewer_model.has_value());
  EXPECT_EQ(
      require_product_artifact_bytes(
          *bundle->value().compiled_products.viewer_model, "urdf", raw_store),
      require_product_artifact_bytes(
          bundle->value().compiled_products.planning_collision_model, "urdf",
          raw_store));

  ASSERT_EQ(bundle->value().compiled_products.symbol_role_maps.size(), 1U);
  const std::string symbol_map = require_product_artifact_bytes(
      bundle->value().compiled_products.symbol_role_maps.front(),
      detail::kComposedModelSymbolRoleMapArtifactRole, raw_store);
  const CanonicalValue retained_map =
      detail::parse_and_verify_composed_model_symbol_role_map(symbol_map);
  ASSERT_EQ(array_member(retained_map, "control_binding_coordinate_maps").size(),
            bundle->value().cell_graph.control_bindings.size());
  const auto cold_symbol_map = detail::emit_composed_model_symbol_role_map(
      cold_fixture.qualified.compiler_ir());
  const CanonicalValue parsed =
      detail::parse_and_verify_composed_model_symbol_role_map(cold_symbol_map);
  EXPECT_NO_THROW(detail::require_composed_model_symbol_role_map_matches(
      cold_symbol_map, cold_fixture.qualified.compiler_ir()));
  EXPECT_NO_THROW(detail::require_composed_model_symbol_role_map_matches(
      symbol_map, fixture.qualified.compiler_ir()));

  const auto &binding_maps =
      array_member(parsed, "control_binding_coordinate_maps");
  ASSERT_EQ(binding_maps.size(), 1U);
  EXPECT_EQ(string_member(binding_maps.front(), "control_binding_id"),
            test::kLite6ArmBindingId);
  EXPECT_EQ(string_member(binding_maps.front(), "controllable_component_id"),
            test::kLite6ControllableComponentId);
  EXPECT_EQ(string_member(binding_maps.front(), "command_target_id"),
            test::kLite6ArmTargetId);
  const auto &resources = array_member(binding_maps.front(), "resources");
  ASSERT_EQ(resources.size(), 6U);
  for (std::size_t index = 0U; index < resources.size(); ++index) {
    EXPECT_EQ(string_member(resources[index], "resource_role_id"), "joints");
    EXPECT_EQ(string_member(resources[index], "resource_id"),
              "lite6_joint_" + std::to_string(index + 1U));
    ASSERT_EQ(array_member(resources[index], "coordinates").size(), 1U);
  }

  const auto &targets = array_member(parsed, "target_role_maps");
  ASSERT_EQ(targets.size(), 1U);
  EXPECT_EQ(string_member(targets.front(), "controllable_component_id"),
            test::kLite6ControllableComponentId);
  EXPECT_EQ(string_member(targets.front(), "command_target_id"),
            test::kLite6ArmTargetId);
  EXPECT_EQ(array_member(targets.front(), "model_groups").size(), 1U);
  const auto &frames = array_member(targets.front(), "frames");
  ASSERT_EQ(frames.size(), 3U);
  EXPECT_EQ(string_member(frames[0], "frame_role_id"), "base");
  EXPECT_EQ(string_member(frames[1], "frame_role_id"), "flange");
  EXPECT_EQ(string_member(frames[2], "frame_role_id"), "tcp");
  EXPECT_EQ(array_member(targets.front(), "collision_bodies").size(), 1U);
}

TEST(RetainedBundleMaterializer,
     NativeMotionLite6RetainsExactRuntimeProductAndProviderEndpoint) {
  const auto fixture = test::make_lite6_ros2_control_motion_retention_fixture();
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  EXPECT_EQ(bundle->value().catalogs.records.size(), 12U);
  EXPECT_EQ(catalog_store.size(), 12U);
  ASSERT_EQ(bundle->value().cell_graph.runtime_units.size(), 1U);
  const auto &unit = bundle->value().cell_graph.runtime_units.front();
  ASSERT_EQ(bundle->value().capability_provider_bindings.size(), 3U);
  const std::vector<std::string> capability_ids = [&] {
    std::vector<std::string> result;
    for (const auto &binding : bundle->value().capability_provider_bindings) {
      result.push_back(binding.capability_id.value());
    }
    return result;
  }();
  EXPECT_EQ(capability_ids,
            (std::vector<std::string>{"planning.motion.cartesian_path",
                                      "planning.motion.joint_goal",
                                      "planning.motion.pose_goal"}));
  const auto provider_it = std::find_if(
      bundle->value().capability_provider_bindings.begin(),
      bundle->value().capability_provider_bindings.end(),
      [](const auto &binding) {
        return binding.capability_id.value() == "planning.motion.joint_goal";
      });
  ASSERT_NE(provider_it, bundle->value().capability_provider_bindings.end());
  const auto &provider = *provider_it;
  EXPECT_EQ(provider.capability_provider_binding_id.value(),
            "lite6_joint_planner_binding");
  EXPECT_EQ(provider.capability_id.value(), "planning.motion.joint_goal");
  EXPECT_EQ(provider.provider_manifest_id.value(),
            "lite6_ros2_control_joint_planner");
  ASSERT_EQ(provider.endpoint_bindings.size(), 1U);
  const auto &endpoint = provider.endpoint_bindings.front();
  EXPECT_EQ(endpoint.endpoint_role_id.value(),
            detail::kRos2ControlExecutionRuntimeProjectionEndpointRoleId);
  EXPECT_EQ(endpoint.source_kind,
            core::CapabilityEndpointSourceKind::kRuntimeUnit);
  ASSERT_TRUE(std::holds_alternative<core::RuntimeUnitId>(endpoint.source_id));
  EXPECT_EQ(std::get<core::RuntimeUnitId>(endpoint.source_id).value(),
            test::kLite6Ros2ControlExecutionRuntimeId);
  EXPECT_EQ(endpoint.source_endpoint_role_id.value(),
            detail::kRos2ControlExecutionRuntimeProjectionEndpointRoleId);
  const auto &deployment_providers = fixture.qualified.deployment()
                                         .applicability_inputs()
                                         .value()
                                         .capability_provider_bindings;
  const auto deployment_provider_it = std::find_if(
      deployment_providers.begin(), deployment_providers.end(),
      [](const auto &binding) {
        return binding.capability_id.value() == "planning.motion.joint_goal";
      });
  ASSERT_NE(deployment_provider_it, deployment_providers.end());
  EXPECT_EQ(provider, *deployment_provider_it);

  ASSERT_EQ(bundle->value().compiled_products.runtime_unit_products.size(), 1U);
  const auto &group =
      bundle->value().compiled_products.runtime_unit_products.front();
  EXPECT_EQ(group.runtime_unit_id.value(),
            test::kLite6Ros2ControlExecutionRuntimeId);
  ASSERT_EQ(group.products.size(), 1U);
  const auto &product = group.products.front();
  EXPECT_EQ(product.product_kind,
            core::RetainedProductKind::kRuntimeUnitConfiguration);
  EXPECT_EQ(product.product_id.value(),
            detail::ros2_control_execution_runtime_product_id(group.runtime_unit_id));
  EXPECT_EQ(product.product_contract_id.value(),
            detail::kRos2ControlExecutionRuntimeProductContractId);
  EXPECT_EQ(product.product_contract_revision,
            detail::ros2_control_execution_runtime_product_contract_revision());
  ASSERT_EQ(product.artifact_refs.size(), 2U);
  const std::string manifest = require_product_artifact_bytes(
      product, detail::kRos2ControlExecutionRuntimeManifestArtifactRole, raw_store);
  const std::string launch_descriptor = require_product_artifact_bytes(
      product, detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole,
      raw_store);
  EXPECT_EQ(launch_descriptor,
            detail::ros2_control_execution_runtime_launch_descriptor());
  const auto parsed_manifest =
      detail::parse_and_verify_ros2_control_execution_runtime_manifest(manifest);
  const auto& retained_liveness =
      execution_runtime_contract::manager_liveness_policy();
  EXPECT_EQ(
      string_member(member(parsed_manifest, "manager_liveness_policy"),
                    "runtime_policy_id"),
      retained_liveness.runtime_policy_id.value());
  EXPECT_EQ(
      string_member(member(parsed_manifest, "manager_liveness_policy"),
                    "runtime_policy_revision"),
      retained_liveness.runtime_policy_revision.value());
  EXPECT_EQ(
      canonical_json(member(parsed_manifest, "configuration")),
      "{}");
  EXPECT_NO_THROW(
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          manifest, launch_descriptor, bundle->value(), group.runtime_unit_id));
  static_cast<void>(ros2_control_execution_runtime_contract::require_configuration(
      *bundle, group.runtime_unit_id));
  const auto injected_controller_configuration = replace_object_member(
      parsed_manifest, "configuration",
      CanonicalValue(CanonicalValue::Object{
          {"execution_tick_hz", CanonicalValue(100.0)},
      }));
  const std::string changed_configuration =
      canonical_json(injected_controller_configuration);
  EXPECT_THROW(static_cast<void>(
                   detail::parse_and_verify_ros2_control_execution_runtime_manifest(
                       changed_configuration)),
               RetainedBundleError);
  EXPECT_THROW(
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          changed_configuration, launch_descriptor, bundle->value(),
          group.runtime_unit_id),
      RetainedBundleError);
  const std::string changed_liveness = replace_once(
      manifest, "\"heartbeat_period_ns\":250000000",
      "\"heartbeat_period_ns\":250000001");
  EXPECT_THROW(static_cast<void>(
                   detail::parse_and_verify_ros2_control_execution_runtime_manifest(
                       changed_liveness)),
               RetainedBundleError);

  // Composition may change controller inputs while retaining the same host.
  // The ROS manifest is consequently byte-identical for this runtime unit.
  const auto composed_fixture =
      test::make_lite6_ros2_control_shared_consumer_retention_fixture({}, 2U);
  const test::Lite6ControllableCompilerInputArtifactSource composed_inputs(
      composed_fixture);
  test::InMemoryCatalogArtifactStoreFixture composed_catalog_store;
  test::InMemoryRawArtifactStoreFixture composed_raw_store;
  const auto composed_bundle = detail::RetainedBundleMaterializer::materialize(
      composed_fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(),
      composed_inputs,
      composed_fixture.structural_preparations(), composed_catalog_store,
      composed_raw_store);
  ASSERT_NE(composed_bundle, nullptr);
  ASSERT_EQ(composed_bundle->value().cell_graph.runtime_units.size(), 1U);
  const auto &composed_unit =
      composed_bundle->value().cell_graph.runtime_units.front();
  EXPECT_EQ(composed_unit.runtime_unit_id, group.runtime_unit_id);
  EXPECT_NE(composed_unit.configuration, unit.configuration);
  const auto &composed_group = composed_bundle->value()
                                    .compiled_products.runtime_unit_products
                                    .front();
  const std::string composed_manifest = require_product_artifact_bytes(
      composed_group.products.front(),
      detail::kRos2ControlExecutionRuntimeManifestArtifactRole,
      composed_raw_store);
  EXPECT_EQ(composed_manifest, manifest);

  test::InMemoryCatalogArtifactStoreFixture second_catalog_store;
  test::InMemoryRawArtifactStoreFixture second_raw_store;
  const auto regenerated = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), second_catalog_store,
      second_raw_store);
  ASSERT_NE(regenerated, nullptr);
  EXPECT_EQ(regenerated->retained_bundle_id(), bundle->retained_bundle_id());
  EXPECT_EQ(regenerated->canonical_bytes(), bundle->canonical_bytes());
}

TEST(RetainedBundleMaterializer,
     GripperOnlyRosCellRetainsAUnitWithoutAnyMotionConfiguration) {
  const auto fixture =
      test::make_lite_gripper_only_retention_fixture(test::kWithLifecycleOffers);
  const test::Lite6ControllableCompilerInputArtifactSource input_source(fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);
  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  const auto &graph = bundle->value().cell_graph;
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  const auto &unit = graph.runtime_units.front();
  EXPECT_TRUE(std::none_of(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [](const auto &binding) {
        return binding.capability_id.value() == "execution.trajectory.execute" ||
               binding.command_target_id.value() == test::kLite6ArmTargetId;
      }));
  EXPECT_TRUE(std::any_of(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &binding) {
        return binding.capability_id.value() == "tool.gripper.command" &&
               binding.provider_runtime_unit_id == unit.runtime_unit_id;
      }));
  EXPECT_TRUE(bundle->value().capability_provider_bindings.empty());
  ASSERT_EQ(bundle->value().desired_control_configurations.size(), 1U);
  const auto configuration =
      ros2_control_execution_runtime_contract::require_configuration(
          *bundle, unit.runtime_unit_id);
  EXPECT_TRUE(configuration.initial_joint_positions_rad.empty());
  EXPECT_TRUE(configuration.motion_target_limits.empty());
  // ROS has no authored direct-control-limit field.
  EXPECT_EQ(unit.configuration.find("direct_control_joint_limits"),
            unit.configuration.end());
  auto with_native_direct_limits = unit.configuration;
  with_native_direct_limits.emplace("direct_control_joint_limits",
      core::JsonValueDTO{core::JsonValueDTO::Array{}});
  EXPECT_FALSE(detail::ros2_control_execution_runtime_configuration_is_qualified(
      with_native_direct_limits));
  ASSERT_EQ(configuration.gripper_target_configurations.size(), 1U);
  EXPECT_EQ(configuration.gripper_target_configurations.front().command_target_id,
            core::CommandTargetId(std::string(test::kLite6GripperTargetId)));
  ASSERT_EQ(configuration.controller_profiles.size(), 1U);
  EXPECT_EQ(configuration.controller_profiles.front().controllable_component_id,
            graph.controllable_components.front().controllable_component_id);
  EXPECT_EQ(configuration.controller_profiles.front().lane, "mock");
  EXPECT_EQ(configuration.controller_profiles.front().model_family_id,
            "lite_gripper");
}

TEST(RetainedBundleMaterializer,
     ExecutionMotionLimitDecoderPreservesCapsAndRefusesInvalidOrDuplicateRows) {
  const auto row = [](const double maximum) {
    return core::JsonValueDTO{core::JsonObjectDTO{
        {"controllable_component_id",
         core::JsonValueDTO{std::string("isaac_ur10e_light_robot")}},
        {"command_target_id", core::JsonValueDTO{std::string("arm")}},
        {"maximum_tcp_linear_speed_mps", core::JsonValueDTO{maximum}},
    }};
  };
  const auto valid = detail::decode_execution_motion_target_limits(
      core::JsonValueDTO::Array{row(0.25)});
  ASSERT_EQ(valid.size(), 1U);
  EXPECT_DOUBLE_EQ(valid.front().maximum_tcp_linear_speed_mps, 0.25);
  EXPECT_FALSE(valid.front().ordinary_motion_jerk.enabled);
  for (const double invalid :
       {0.0, -0.1, 1001.0, std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_THROW(
        static_cast<void>(detail::decode_execution_motion_target_limits(
            core::JsonValueDTO::Array{row(invalid)})),
        RetainedBundleError);
  }
  EXPECT_THROW(static_cast<void>(detail::decode_execution_motion_target_limits(
                   core::JsonValueDTO::Array{row(0.25), row(0.25)})),
               RetainedBundleError);
  auto missing = row(0.25);
  std::get<core::JsonObjectDTO>(missing.value).erase("command_target_id");
  EXPECT_THROW(static_cast<void>(detail::decode_execution_motion_target_limits(
                   core::JsonValueDTO::Array{missing})),
               RetainedBundleError);
  // The row is closed: an unknown member refuses.
  auto extra = row(0.25);
  std::get<core::JsonObjectDTO>(extra.value).emplace(
      "unknown_member", core::JsonValueDTO{std::string("x")});
  EXPECT_THROW(static_cast<void>(detail::decode_execution_motion_target_limits(
                   core::JsonValueDTO::Array{extra})),
               RetainedBundleError);
  // The authored ordinary-jerk switch and typed values decode.
  auto jerk = row(0.25);
  std::get<core::JsonObjectDTO>(jerk.value).emplace(
      "ordinary_motion_jerk",
      core::JsonValueDTO{core::JsonObjectDTO{
          {"enabled", core::JsonValueDTO{true}},
          {"preset_reference_time_s", core::JsonValueDTO{0.5}},
          {"joint_limits",
           core::JsonValueDTO{core::JsonValueDTO::Array{core::JsonValueDTO{
               core::JsonObjectDTO{
                   {"source_joint_name", core::JsonValueDTO{std::string("joint1")}},
                   {"override_rad_s3", core::JsonValueDTO{40.0}}}}}}}}});
  const auto decoded =
      detail::decode_execution_motion_target_limits(core::JsonValueDTO::Array{jerk});
  EXPECT_EQ(decoded.front().ordinary_motion_jerk,
            (execution_runtime_contract::OrdinaryMotionJerkSettings{
                true, 0.5, {{"joint1", 40.0}}}));
}

// A controller profile row is closed: the removed robot-row `tool_payload`
// (a tool's payload now comes from its model) is refused, not ignored.
TEST(RetainedBundleMaterializer, Ros2ControlControllerProfileRefusesToolPayload) {
  const auto fixture =
      test::make_lite_gripper_only_retention_fixture(test::kWithLifecycleOffers);
  const test::Lite6ControllableCompilerInputArtifactSource input_source(fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);
  ASSERT_NE(bundle, nullptr);
  const auto &unit = bundle->value().cell_graph.runtime_units.front();
  ASSERT_TRUE(detail::ros2_control_execution_runtime_configuration_is_qualified(
      unit.configuration));
  using Json = core::JsonValueDTO;
  auto with_tool_payload = unit.configuration;
  auto &profiles = std::get<Json::Array>(
      with_tool_payload.at("controller_profiles").value);
  ASSERT_EQ(profiles.size(), 1U);
  std::get<core::JsonObjectDTO>(profiles.front().value)
      .emplace("tool_payload",
               Json{core::JsonObjectDTO{
                   {"mass_kg", Json{0.3}},
                   {"center_of_mass_m",
                    Json{Json::Array{Json{0.0}, Json{0.0}, Json{0.05}}}}}});
  EXPECT_FALSE(detail::ros2_control_execution_runtime_configuration_is_qualified(
      with_tool_payload));
}

TEST(RetainedBundleMaterializer,
     Ros2ControlConfigurationRejectsSchemaValidLimitForAnUnhostedTarget) {
  const auto fixture = test::make_lite6_ros2_control_motion_retention_fixture(
      false, "unhosted_arm");
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);
  ASSERT_NE(bundle, nullptr);
  const auto &unit = bundle->value().cell_graph.runtime_units.front();
  EXPECT_TRUE(detail::ros2_control_execution_runtime_configuration_is_qualified(
      unit.configuration));
  EXPECT_THROW(
      static_cast<void>(
          ros2_control_execution_runtime_contract::require_configuration(
              *bundle, unit.runtime_unit_id)),
      RetainedBundleError);
}

[[nodiscard]] CanonicalValue authored_fault_stop_profile_rows(
    std::string_view target_id, std::string_view stop_mode,
    std::optional<bool> natural_completion = std::nullopt) {
  CanonicalValue::Array limits;
  for (const std::string_view joint :
       {"joint1", "joint2", "joint3", "joint4", "joint5", "joint6"}) {
    limits.push_back(CanonicalValue(CanonicalValue::Object{
        {"source_joint_name", CanonicalValue(std::string(joint))},
        {"max_deceleration_rad_s2", CanonicalValue(10.0)},
    }));
  }
  CanonicalValue::Object row{
      {"controllable_component_id",
       CanonicalValue(std::string(test::kLite6ControllableComponentId))},
      {"command_target_id", CanonicalValue(std::string(target_id))},
      {"profile_id", CanonicalValue(stop_mode == "spring_back_fast"
                                         ? "fault_spring_back_fast"
                                         : "fault_spring_back_hold")},
      {"stop_mode", CanonicalValue(std::string(stop_mode))},
      {"require_backend_confirmation", CanonicalValue(true)},
  };
  if (stop_mode != "hard_deceleration") {
    row.emplace_back("deceleration_time_s", CanonicalValue(1.0));
    row.emplace_back("per_joint_max_deceleration_rad_s2",
                     CanonicalValue(std::move(limits)));
  }
  if (natural_completion.has_value()) {
    row.emplace_back("allow_natural_completion_on_short_path",
                     CanonicalValue(*natural_completion));
  }
  return CanonicalValue(CanonicalValue::Array{CanonicalValue(std::move(row))});
}

[[nodiscard]] ros2_control_execution_runtime_contract::Configuration
ros2_configuration_with_fault_rows(const CanonicalValue &rows) {
  const auto fixture = test::make_lite6_ros2_control_motion_retention_fixture(
      false, test::kLite6ArmTargetId, rows);
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);
  if (bundle == nullptr) {
    throw std::logic_error("ROS fault-profile fixture did not materialize");
  }
  return ros2_control_execution_runtime_contract::require_configuration(
      *bundle, bundle->value().cell_graph.runtime_units.front().runtime_unit_id);
}

TEST(RetainedBundleMaterializer,
     Ros2ControlConfigurationReadsAuthoredFaultStopProfileDirectly) {
  const auto configuration = ros2_configuration_with_fault_rows(
      authored_fault_stop_profile_rows(test::kLite6ArmTargetId,
                                       "spring_back_hold"));
  ASSERT_EQ(configuration.default_fault_stop_profiles.size(), 1U);
  const auto &authored = configuration.default_fault_stop_profiles.front();
  EXPECT_EQ(authored.controllable_component_id.value(),
            test::kLite6ControllableComponentId);
  EXPECT_EQ(authored.command_target_id.value(), test::kLite6ArmTargetId);
  const auto &profile = authored.profile;
  EXPECT_EQ(profile.profile_id, "fault_spring_back_hold");
  EXPECT_EQ(profile.stop_mode, core::MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(profile.deceleration_time_s, std::optional<double>(1.0));
  ASSERT_EQ(profile.per_joint_max_deceleration.size(), 6U);
  for (const auto &[joint, limit] : profile.per_joint_max_deceleration) {
    SCOPED_TRACE(joint);
    ASSERT_TRUE(std::holds_alternative<core::AngularAccelerationDTO>(limit));
    EXPECT_DOUBLE_EQ(std::get<core::AngularAccelerationDTO>(limit).value_rad_s2,
                     10.0);
  }
  // Normalized as every authored profile: natural completion only for soft.
  EXPECT_FALSE(profile.allow_natural_completion_on_short_path);
  EXPECT_TRUE(profile.require_backend_confirmation);
  // main shaped every Stop ramp with its default jerk; no per-joint override.
  EXPECT_EQ(profile.jerk_limits_enabled, std::optional<bool>(true));
  EXPECT_TRUE(profile.per_joint_max_jerk.empty());
  EXPECT_FALSE(profile.per_joint_settle_velocity_threshold.has_value());
  EXPECT_TRUE(profile.fallback_stop_modes.empty());

  const auto fast = ros2_configuration_with_fault_rows(
      authored_fault_stop_profile_rows(test::kLite6ArmTargetId,
                                       "spring_back_fast"));
  ASSERT_EQ(fast.default_fault_stop_profiles.size(), 1U);
  auto expected_fast = profile;
  expected_fast.profile_id = "fault_spring_back_fast";
  expected_fast.stop_mode = core::MotionInterruptStopMode::kSpringBackFast;
  EXPECT_EQ(fast.default_fault_stop_profiles.front().profile, expected_fast);

  const auto soft = ros2_configuration_with_fault_rows(
      authored_fault_stop_profile_rows(test::kLite6ArmTargetId,
                                       "soft_on_path"));
  ASSERT_EQ(soft.default_fault_stop_profiles.size(), 1U);
  EXPECT_TRUE(soft.default_fault_stop_profiles.front()
                  .profile.allow_natural_completion_on_short_path);

  const auto hard = ros2_configuration_with_fault_rows(
      authored_fault_stop_profile_rows(test::kLite6ArmTargetId,
                                       "hard_deceleration"));
  ASSERT_EQ(hard.default_fault_stop_profiles.size(), 1U);
  EXPECT_EQ(hard.default_fault_stop_profiles.front().profile.stop_mode,
            core::MotionInterruptStopMode::kHardDeceleration);
  EXPECT_EQ(hard.default_fault_stop_profiles.front().profile.jerk_limits_enabled,
            std::optional<bool>(false));
}

TEST(RetainedBundleMaterializer,
     Ros2ControlConfigurationRejectsInvalidAuthoredFaultStopProfile) {
  EXPECT_THROW(static_cast<void>(ros2_configuration_with_fault_rows(
                   authored_fault_stop_profile_rows("unhosted_arm",
                                                    "spring_back_hold"))),
               RetainedBundleError);
  EXPECT_THROW(static_cast<void>(ros2_configuration_with_fault_rows(
                   authored_fault_stop_profile_rows(
                       test::kLite6ArmTargetId, "hard_deceleration", true))),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     Ros2ControlMotionLite6RetainsDistinctExactRuntimeProduct) {
  const auto fixture =
      test::make_lite6_ros2_control_motion_retention_fixture();
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  EXPECT_EQ(bundle->value().catalogs.records.size(), 12U);
  EXPECT_EQ(catalog_store.size(), 12U);
  ASSERT_EQ(bundle->value().cell_graph.runtime_units.size(), 1U);
  const auto &runtime_unit = bundle->value().cell_graph.runtime_units.front();
  EXPECT_EQ(runtime_unit.runtime_unit_id.value(),
            test::kLite6Ros2ControlExecutionRuntimeId);
  EXPECT_EQ(runtime_unit.runtime_profile_id.value(),
            detail::kRos2ControlExecutionRuntimeProfileId);
  EXPECT_EQ(runtime_unit.runtime_profile_revision.value(),
            detail::ros2_control_execution_runtime_profile_revision());
  ASSERT_EQ(bundle->value().cell_graph.control_bindings.size(), 1U);
  EXPECT_EQ(bundle->value().cell_graph.control_bindings.front()
                .state_authority_class,
            core::StateAuthorityClass::kCommanded);

  ASSERT_EQ(bundle->value().compiled_products.runtime_unit_products.size(),
            1U);
  const auto &group =
      bundle->value().compiled_products.runtime_unit_products.front();
  ASSERT_EQ(group.products.size(), 1U);
  const auto &product = group.products.front();
  EXPECT_EQ(product.product_kind,
            core::RetainedProductKind::kRuntimeUnitConfiguration);
  EXPECT_EQ(product.product_id.value(),
            detail::ros2_control_execution_runtime_product_id(
                group.runtime_unit_id));
  EXPECT_EQ(product.product_contract_id.value(),
            detail::kRos2ControlExecutionRuntimeProductContractId);
  EXPECT_EQ(
      product.product_contract_revision,
      detail::ros2_control_execution_runtime_product_contract_revision());
  ASSERT_EQ(product.artifact_refs.size(), 2U);
  const std::string manifest = require_product_artifact_bytes(
      product, detail::kRos2ControlExecutionRuntimeManifestArtifactRole,
      raw_store);
  const std::string launch_descriptor = require_product_artifact_bytes(
      product, detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole,
      raw_store);
  EXPECT_EQ(launch_descriptor,
            detail::ros2_control_execution_runtime_launch_descriptor());
  const CanonicalValue parsed_manifest =
      detail::parse_and_verify_ros2_control_execution_runtime_manifest(
          manifest);
  const auto &liveness =
      execution_runtime_contract::manager_liveness_policy();
  const auto &retained_liveness = member(parsed_manifest, "manager_liveness_policy");
  EXPECT_EQ(string_member(retained_liveness, "runtime_policy_id"),
            liveness.runtime_policy_id.value());
  EXPECT_EQ(string_member(retained_liveness, "runtime_policy_revision"),
            liveness.runtime_policy_revision.value());
  EXPECT_EQ(std::get<double>(
                member(retained_liveness, "heartbeat_period_ns").storage()),
            static_cast<double>(liveness.heartbeat_period_ns));
  EXPECT_EQ(std::get<double>(
                member(retained_liveness, "expiry_timeout_ns").storage()),
            static_cast<double>(liveness.expiry_timeout_ns));
  EXPECT_EQ(string_member(retained_liveness, "local_safe_state_contract_id"),
            liveness.local_safe_state_contract_id.value());
  EXPECT_EQ(string_member(retained_liveness, "local_safe_state_contract_revision"),
            liveness.local_safe_state_contract_revision.value());
  const auto changed_liveness = replace_once(
      manifest, "\"heartbeat_period_ns\":250000000",
      "\"heartbeat_period_ns\":250000001");
  EXPECT_THROW(static_cast<void>(
      detail::parse_and_verify_ros2_control_execution_runtime_manifest(
          changed_liveness)),
      RetainedBundleError);
  EXPECT_EQ(
      canonical_json(member(parsed_manifest, "configuration")),
      "{}");
  EXPECT_NO_THROW(
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          manifest, launch_descriptor, bundle->value(), group.runtime_unit_id));

  const auto runtime_configuration =
      ros2_control_execution_runtime_contract::require_configuration(
          *bundle, group.runtime_unit_id);
  ASSERT_EQ(runtime_configuration.controller_profiles.size(), 1U);
  EXPECT_EQ(runtime_configuration.controller_profiles.front()
                .controllable_component_id.value(),
            test::kLite6ControllableComponentId);
  EXPECT_EQ(runtime_configuration.controller_profiles.front().lane, "isaac");
  EXPECT_EQ(runtime_configuration.controller_profiles.front().lane_row,
            R"({"adapter":"isaac"})");
  EXPECT_EQ(runtime_configuration.controller_profiles.front().model_family_id,
            "lite6");
  // Not authored: the soft Stop lead-in keeps the default (command position).
  EXPECT_FALSE(runtime_configuration.controller_profiles.front()
                   .stop_lead_in_anchor.has_value());
  ASSERT_EQ(runtime_configuration.motion_target_limits.size(), 1U);
  const auto &motion_limit = runtime_configuration.motion_target_limits.front();
  EXPECT_EQ(motion_limit.controllable_component_id.value(),
            test::kLite6ControllableComponentId);
  EXPECT_EQ(motion_limit.command_target_id.value(), "arm");
  EXPECT_DOUBLE_EQ(motion_limit.maximum_tcp_linear_speed_mps, 0.15);
  EXPECT_FALSE(motion_limit.ordinary_motion_jerk.enabled);
  EXPECT_EQ(runtime_configuration.planner_rng_seed, 12345U);
  EXPECT_EQ(runtime_configuration.initial_joint_positions_rad,
            (std::vector<double>{0.0, 0.0, 1.57, 0.0, 0.0, 0.0}));
  EXPECT_EQ(runtime_configuration.execution_tick_hz, 100U);
  EXPECT_DOUBLE_EQ(runtime_configuration.collision_monitor_period_ms, 100.0);
  EXPECT_EQ(runtime_configuration.runtime_collision_immediate_query_budget_ms,
            75.0);
  EXPECT_EQ(runtime_configuration.runtime_collision_lookahead_query_budget_ms,
            75.0);
  EXPECT_EQ(runtime_configuration.runtime_collision_lookahead_cycle_budget_ms,
            350.0);
  EXPECT_EQ(runtime_configuration.runtime_collision_lookahead_s, 2.0);
  EXPECT_EQ(runtime_configuration.runtime_collision_lookahead_max_samples,
            32U);
  EXPECT_EQ(runtime_configuration.runtime_collision_lookahead_workers, 4U);
  EXPECT_EQ(runtime_configuration.runtime_dynamic_body_max_age_cap_ms,
            1000.0);
  EXPECT_EQ(runtime_configuration.validation_collision_workers, 4U);
  // Not declared: the kernel's collision clearance, check-step and
  // fresh-plan check defaults.
  EXPECT_FALSE(runtime_configuration.collision_clearance_m.has_value());
  EXPECT_FALSE(runtime_configuration.collision_check_step_rad.has_value());
  EXPECT_FALSE(runtime_configuration.collision_check_fresh_plans.has_value());
  EXPECT_TRUE(runtime_configuration.default_fault_stop_profiles.empty());

  const auto changed_configuration = canonical_json(replace_object_member(
      parsed_manifest, "configuration", CanonicalValue(CanonicalValue::Object{
          {"lane", CanonicalValue("mock")}})));
  EXPECT_THROW(static_cast<void>(
      detail::parse_and_verify_ros2_control_execution_runtime_manifest(
          changed_configuration)), RetainedBundleError);
  EXPECT_THROW(
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          changed_configuration, launch_descriptor, bundle->value(),
          group.runtime_unit_id),
      RetainedBundleError);
  EXPECT_NO_THROW(static_cast<void>(
      detail::RetainedBundleReloadAuthority::reload(bundle->canonical_bytes())));
}

TEST(RetainedBundleMaterializer,
     LogicalSceneRuntimeRequirementsMatchNativeAndRos2ControlOffers) {
  // The native positive-family leg retires with native execution admission.
  {
    const auto fixture =
        test::make_lite6_ros2_control_motion_retention_fixture();
    const auto& applicability = fixture.qualified.compiler_ir().applicability();
    const auto& graph = applicability.composition().graph();
    ASSERT_EQ(graph.runtime_units.size(), 1U);
    const auto coverage = make_logical_scene_runtime_coverage(
        graph.runtime_units.front(), graph.cell_id);

    const auto requirements = detail::derive_runtime_contract_requirements(
        graph, coverage.bindings, coverage.capabilities, coverage.endpoints,
        applicability.runtime_profiles(), {});
    ASSERT_EQ(requirements.size(), 1U);
    const auto& requirement = requirements.front().requirement;
    ASSERT_EQ(requirement.required_capabilities.size(),
              runtime_contract_capabilities::kLogicalSceneServices.size());

    std::vector<core::RuntimeCapabilityContractRefDTO> expected_capabilities;
    expected_capabilities.reserve(
        runtime_contract_capabilities::kLogicalSceneServices.size());
    for (const auto capability_id :
         runtime_contract_capabilities::kLogicalSceneServices) {
      const auto* descriptor =
          core::find_builtin_capability_definition(capability_id);
      ASSERT_NE(descriptor, nullptr);
      expected_capabilities.push_back({
          core::CapabilityId(std::string(capability_id)),
          core::CapabilityDefinitionRevision(
              std::string(descriptor->capability_definition_revision)),
      });
    }
    EXPECT_EQ(requirement.required_capabilities, expected_capabilities);

    const auto& selected_offer =
        detail::require_runtime_contract_for_unit(graph.runtime_units.front())
            .offer();
    EXPECT_TRUE(
        core::CheckRuntimeContractCompatibility(selected_offer, requirement)
            .ok());
    ASSERT_EQ(coverage.endpoints.front().endpoint_role_id.value(),
              "logical_scene_services");
    const auto* source = std::get_if<RuntimeEndpointSourceTemplate>(
        &coverage.endpoints.front().source);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->runtime_unit_id,
              graph.runtime_units.front().runtime_unit_id);
  }
}

TEST(RetainedBundleMaterializer,
     ExternalProviderEndpointsDoNotContributeRuntimeRequirements) {
  const auto fixture = test::make_lite6_ros2_control_motion_retention_fixture();
  const auto& applicability = fixture.qualified.compiler_ir().applicability();
  const auto& graph = applicability.composition().graph();
  ASSERT_EQ(graph.runtime_units.size(), 1U);

  std::vector<core::CapabilityProviderBindingDTO> bindings(
      fixture.qualified.deployment()
          .applicability_inputs()
          .value()
          .capability_provider_bindings.begin(),
      fixture.qualified.deployment()
          .applicability_inputs()
          .value()
          .capability_provider_bindings.end());
  std::vector<LogicalProviderEndpointTemplate> endpoints(
      applicability.provider_endpoints().begin(),
      applicability.provider_endpoints().end());

  const auto binding_for = [&](std::string_view capability_id)
      -> core::CapabilityProviderBindingDTO* {
    const auto found = std::find_if(
        bindings.begin(), bindings.end(), [capability_id](const auto& binding) {
          return binding.capability_id.value() == capability_id;
        });
    return found == bindings.end() ? nullptr : &*found;
  };
  auto* mixed_binding = binding_for("planning.motion.joint_goal");
  auto* external_only_binding = binding_for("planning.motion.pose_goal");
  ASSERT_NE(mixed_binding, nullptr);
  ASSERT_NE(external_only_binding, nullptr);

  const auto endpoint_for =
      [&](const core::CapabilityProviderBindingDTO& binding) {
        return std::find_if(
            endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
              return endpoint.capability_provider_binding_id ==
                         binding.capability_provider_binding_id &&
                     endpoint.capability_provider_binding_revision ==
                         binding.capability_provider_binding_revision;
            });
      };
  const auto external_source =
      [](const LogicalProviderEndpointTemplate& endpoint,
         std::string_view alias, std::string_view role)
      -> LogicalEndpointSourceTemplate {
    return LogicalEndpointSourceTemplate(ExternalEndpointSourceTemplate{
        core::ExternalResourceAlias(std::string(alias)),
        core::ExternalResourceKind::kServiceEndpoint,
        endpoint.provider_instance_id,
        endpoint.provider_manifest_id,
        endpoint.provider_manifest_revision,
        core::EndpointRoleId(std::string(role)),
        core::SiteOverlayValueRef("test.external"),
        core::SiteOverlayId("test_overlay"),
        core::SiteOverlayRevision(std::string(kZeroDigest)),
        core::ContentDigest(std::string(kZeroDigest)),
    });
  };

  const auto mixed_endpoint = endpoint_for(*mixed_binding);
  ASSERT_NE(mixed_endpoint, endpoints.end());
  ASSERT_TRUE(std::holds_alternative<RuntimeEndpointSourceTemplate>(
      mixed_endpoint->source));
  mixed_binding->endpoint_bindings.push_back(
      {core::EndpointRoleId("mixed_external"),
       core::CapabilityEndpointSourceKind::kExternalResource,
       core::ExternalResourceAlias("mixed_external"),
       core::EndpointRoleId("mixed_external")});
  auto mixed_external_endpoint = *mixed_endpoint;
  mixed_external_endpoint.endpoint_role_id =
      core::EndpointRoleId("mixed_external");
  mixed_external_endpoint.source =
      external_source(*mixed_endpoint, "mixed_external", "mixed_external");
  mixed_external_endpoint.logical_endpoint_symbol = "test_mixed_external";
  endpoints.push_back(std::move(mixed_external_endpoint));

  const auto external_only_endpoint = endpoint_for(*external_only_binding);
  ASSERT_NE(external_only_endpoint, endpoints.end());
  external_only_binding->endpoint_bindings = {
      {core::EndpointRoleId("external_only"),
       core::CapabilityEndpointSourceKind::kExternalResource,
       core::ExternalResourceAlias("external_only"),
       core::EndpointRoleId("external_only")}};
  external_only_endpoint->endpoint_role_id =
      core::EndpointRoleId("external_only");
  external_only_endpoint->source =
      external_source(*external_only_endpoint, "external_only",
                      "external_only");

  ASSERT_EQ(mixed_binding->endpoint_bindings.size(), 2U);
  EXPECT_EQ(std::count_if(
                mixed_binding->endpoint_bindings.begin(),
                mixed_binding->endpoint_bindings.end(), [](const auto& endpoint) {
                  return endpoint.source_kind ==
                         core::CapabilityEndpointSourceKind::kRuntimeUnit;
                }),
            1U);
  EXPECT_EQ(std::count_if(
                mixed_binding->endpoint_bindings.begin(),
                mixed_binding->endpoint_bindings.end(), [](const auto& endpoint) {
                  return endpoint.source_kind ==
                         core::CapabilityEndpointSourceKind::kExternalResource;
                }),
            1U);
  ASSERT_EQ(external_only_binding->endpoint_bindings.size(), 1U);
  EXPECT_EQ(external_only_binding->endpoint_bindings.front().source_kind,
            core::CapabilityEndpointSourceKind::kExternalResource);
  EXPECT_EQ(std::count_if(
                endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
                  return endpoint.capability_provider_binding_id ==
                             mixed_binding->capability_provider_binding_id &&
                         std::holds_alternative<RuntimeEndpointSourceTemplate>(
                             endpoint.source);
                }),
            1U);
  EXPECT_EQ(std::count_if(
                endpoints.begin(), endpoints.end(), [&](const auto& endpoint) {
                  return endpoint.capability_provider_binding_id ==
                             mixed_binding->capability_provider_binding_id &&
                         std::holds_alternative<ExternalEndpointSourceTemplate>(
                             endpoint.source);
                }),
            1U);
  EXPECT_TRUE(std::holds_alternative<ExternalEndpointSourceTemplate>(
      endpoints[static_cast<std::size_t>(
                    std::distance(endpoints.begin(), external_only_endpoint))]
          .source));

  const auto requirements = detail::derive_runtime_contract_requirements(
      graph, bindings, applicability.capabilities(), endpoints,
      applicability.runtime_profiles(), {});
  ASSERT_EQ(requirements.size(), 1U);
  const auto& requirement = requirements.front();
  EXPECT_EQ(requirement.runtime_unit_id,
            graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(requirement.requirement.required_api_contract_ids.size(), 3U);
  EXPECT_EQ(
      requirement.requirement.required_api_contract_ids,
      (std::vector<core::ApiContractId>{
          core::ApiContractId(std::string(
              ros2_control_execution_runtime_contract::kRuntimeRoleApiContractId)),
          core::ApiContractId(std::string(ros2_control_execution_runtime_contract::
                                              kProjectionEndpointApiContractId)),
          core::ApiContractId(std::string(
              ros2_control_execution_runtime_contract::kStatusEndpointApiContractId))}));

  const auto capability_for = [&](std::string_view capability_id)
      -> const LogicalCapabilityTemplate* {
    const auto found = std::find_if(
        applicability.capabilities().begin(), applicability.capabilities().end(),
        [capability_id](const auto& capability) {
          return capability.capability_id.value() == capability_id;
        });
    return found == applicability.capabilities().end() ? nullptr : &*found;
  };
  const auto* control_capability =
      capability_for("execution.trajectory.execute");
  const auto* cartesian_capability =
      capability_for("planning.motion.cartesian_path");
  const auto* joint_capability = capability_for("planning.motion.joint_goal");
  const auto* pose_capability = capability_for("planning.motion.pose_goal");
  ASSERT_NE(control_capability, nullptr);
  ASSERT_NE(cartesian_capability, nullptr);
  ASSERT_NE(joint_capability, nullptr);
  ASSERT_NE(pose_capability, nullptr);
  ASSERT_EQ(requirement.requirement.required_capabilities.size(), 3U);
  EXPECT_EQ(
      requirement.requirement.required_capabilities,
      (std::vector<core::RuntimeCapabilityContractRefDTO>{
          {control_capability->capability_id,
           control_capability->capability_definition_revision},
          {cartesian_capability->capability_id,
           cartesian_capability->capability_definition_revision},
          {joint_capability->capability_id,
           joint_capability->capability_definition_revision}}));
  EXPECT_EQ(std::count_if(
                requirement.requirement.required_capabilities.begin(),
                requirement.requirement.required_capabilities.end(),
                [&](const auto& capability) {
                  return capability.capability_id ==
                         pose_capability->capability_id;
                }),
            0U);
}

TEST(RetainedBundleMaterializer,
     RuntimeContractRequirementsRejectMalformedCoverageAndNestedDigest) {
  const auto fixture =
      test::make_lite6_ros2_control_shared_consumer_retention_fixture({}, 2U);
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);
  ASSERT_NE(bundle, nullptr);

  CanonicalValue missing = bundle->normalized_root();
  auto missing_rows =
      array_member(missing, "runtime_contract_requirements");
  missing_rows.pop_back();
  missing = replace_root_member(
      missing, "runtime_contract_requirements",
      CanonicalValue(std::move(missing_rows)));
  missing = reidentify_retained_bundle(std::move(missing));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(missing))),
               RetainedBundleError);

  CanonicalValue extra = bundle->normalized_root();
  auto extra_rows = array_member(extra, "runtime_contract_requirements");
  extra_rows.push_back(extra_rows.front());
  extra = replace_root_member(
      extra, "runtime_contract_requirements",
      CanonicalValue(std::move(extra_rows)));
  extra = reidentify_retained_bundle(std::move(extra));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(extra))),
               RetainedBundleError);

  CanonicalValue diagnostic_family = bundle->normalized_root();
  auto family_rows =
      array_member(diagnostic_family, "runtime_contract_requirements");
  const auto& original_requirement =
      bundle->value().runtime_contract_requirements.front().requirement;
  auto diagnostic_capabilities = original_requirement.required_capabilities;
  ASSERT_FALSE(diagnostic_capabilities.empty());
  diagnostic_capabilities.front().capability_definition_revision =
      core::CapabilityDefinitionRevision(std::string(kZeroDigest));
  const auto rebuilt_requirement = core::MakeRuntimeContractRequirement(
      core::RuntimeImplementationFamilyId("compatible.alternative.runtime"),
      original_requirement.required_api_contract_ids,
      std::move(diagnostic_capabilities));
  ASSERT_TRUE(rebuilt_requirement.ok());
  const auto encoded_requirement =
      core::EncodeRuntimeContractRequirement(rebuilt_requirement.value());
  ASSERT_TRUE(encoded_requirement.ok());
  auto first_family_row = family_rows.front();
  first_family_row = replace_object_member(
      first_family_row, "requirement",
      parse_strict_json_model_yaml(encoded_requirement.value()).root());
  family_rows.front() = std::move(first_family_row);
  diagnostic_family = replace_root_member(
      diagnostic_family, "runtime_contract_requirements",
      CanonicalValue(std::move(family_rows)));
  diagnostic_family = reidentify_retained_bundle(std::move(diagnostic_family));
  EXPECT_NO_THROW(static_cast<void>(
      detail::RetainedBundleReloadAuthority::reload(
          canonical_json(diagnostic_family))));

  CanonicalValue bad_digest = bundle->normalized_root();
  auto digest_rows = array_member(bad_digest, "runtime_contract_requirements");
  auto first_row = digest_rows.front();
  auto requirement = member(first_row, "requirement");
  requirement = replace_object_member(
      requirement, "requirement_digest", CanonicalValue(std::string(kZeroDigest)));
  first_row = replace_object_member(first_row, "requirement",
                                    std::move(requirement));
  digest_rows.front() = std::move(first_row);
  bad_digest = replace_root_member(
      bad_digest, "runtime_contract_requirements",
      CanonicalValue(std::move(digest_rows)));
  bad_digest = reidentify_retained_bundle(std::move(bad_digest));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(bad_digest))),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     ExecutionHostProductRejectsMutableLaunchAndTransportSeams) {
  const auto fixture = test::make_lite6_ros2_control_motion_retention_fixture();
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  const auto &group =
      bundle->value().compiled_products.runtime_unit_products.front();
  const auto &product = group.products.front();
  const std::string manifest = require_product_artifact_bytes(
      product, detail::kRos2ControlExecutionRuntimeManifestArtifactRole, raw_store);
  const std::string launch_descriptor = require_product_artifact_bytes(
      product, detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole,
      raw_store);

  struct Mutation final {
    std::string_view name;
    std::string from;
    std::string to;
  };
  const std::array<Mutation, 10U> mutations{{
      {"API version", "manyforge.ros2_control_execution_runtime_manifest.v1",
       "manyforge.ros2_control_execution_runtime_manifest.v0"},
      {"lifecycle", "manager_managed", "externally_managed"},
      {"process adapter", "manyforge.installed_execution_target",
       "manyforge.arbitrary_command"},
      {"retrieval contract", "manyforge.retained_raw_artifact",
       "manyforge.mutable_local_artifact"},
      {"status endpoint", "manyforge.ros2_control_execution.status_direct",
       "manyforge.ros2_control_execution.status_discovered"},
      {"projection endpoint", "manyforge.planner_scene_projection.rpc",
       "manyforge.planner_scene_projection.ros_topic"},
      {"logical scene endpoint", "manyforge.logical_scene.services",
       "manyforge.logical_scene.discovered"},
      {"operating system", "\"operating_system\":\"linux\"",
       "\"operating_system\":\"other\""},
      {"launch environment", "manyforge.ros2_control_execution_host.abi.v1",
       "manyforge.caller_selected_environment"},
      {"runtime role", "\"runtime_role_id\":\"ros2_control_execution\"",
       "\"runtime_role_id\":\"foreign_role\""},
  }};
  for (const auto &mutation : mutations) {
    SCOPED_TRACE(mutation.name);
    EXPECT_THROW(static_cast<void>(
                     detail::parse_and_verify_ros2_control_execution_runtime_manifest(
                         replace_once(manifest, mutation.from, mutation.to))),
                 RetainedBundleError);
  }

  const auto parsed_manifest =
      detail::parse_and_verify_ros2_control_execution_runtime_manifest(manifest);
  const auto embedded_offer = detail::decode_runtime_contract_offer_projection(
      member(parsed_manifest, "runtime_contract_offer"),
      "test.runtime_contract_offer");
  auto forged_api_ids = embedded_offer.provided_api_contract_ids;
  forged_api_ids.emplace_back("manyforge.api.forged");
  const auto forged_offer = core::MakeRuntimeContractOffer(
      embedded_offer.runtime_implementation_id, std::move(forged_api_ids),
      embedded_offer.provided_capabilities);
  ASSERT_TRUE(forged_offer.ok());
  const auto forged_manifest = replace_object_member(
      parsed_manifest, "runtime_contract_offer",
      detail::runtime_contract_offer_projection(forged_offer.value()));
  // The record is strict and self-consistent, but the added API has no
  // build-qualified semantic owner and cannot become an admissible offer.
  EXPECT_THROW(static_cast<void>(
                   detail::parse_and_verify_ros2_control_execution_runtime_manifest(
                       canonical_json(forged_manifest))),
               RetainedBundleError);

  CanonicalValue::Object injected =
      parse_strict_json_model_yaml(manifest).root().as_object();
  injected.emplace_back("command", CanonicalValue(CanonicalValue::Array{
                                       CanonicalValue("/tmp/other")}));
  injected.emplace_back("environment",
                        CanonicalValue(CanonicalValue::Object{
                            {"UNQUALIFIED", CanonicalValue("injected")},
                        }));
  injected.emplace_back("socket_path", CanonicalValue("/tmp/discovered"));
  EXPECT_THROW(static_cast<void>(
                   detail::parse_and_verify_ros2_control_execution_runtime_manifest(
                       canonical_json(CanonicalValue(std::move(injected))))),
               RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          detail::parse_and_verify_ros2_control_execution_runtime_manifest(
              std::string(detail::kMaxRos2ControlExecutionRuntimeManifestBytes + 1U,
                          'x'))),
      RetainedBundleError);
  EXPECT_THROW(
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          manifest,
          replace_once(launch_descriptor,
                       detail::kRos2ControlExecutionRuntimeInstalledTargetId,
                       "caller_selected_target"),
          bundle->value(), group.runtime_unit_id),
      RetainedBundleError);
  EXPECT_THROW(
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          manifest, launch_descriptor, bundle->value(),
          core::RuntimeUnitId("different_runtime")),
      RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     AssistantSettingsRetainReloadAndRequalifyExactly) {
  const auto fixture = test::make_lite6_controllable_retention_fixture(
      false, true, 1U, {}, false, false, "resource", false,
      {}, {}, false, false, std::nullopt,
      test::kLite6ArmTargetId, std::nullopt, std::nullopt, {}, true,
      {}, "manyforge.provider.ros2_control", false, true);
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  ASSERT_TRUE(bundle->value().selected_site_overlay_ref.has_value());
  EXPECT_EQ(bundle->value().selected_site_overlay_ref,
            fixture.qualified.compiler_ir()
                .applicability()
                .selected_site_overlay_ref());
  ASSERT_EQ(bundle->value().external_resources.size(), 2U);
  ASSERT_TRUE(bundle->value().assistant.has_value());

  const auto &qualified_assistant =
      fixture.qualified.compiler_ir().applicability().assistant_settings();
  ASSERT_TRUE(qualified_assistant.enabled());
  EXPECT_EQ(*bundle->value().assistant,
            qualified_assistant.retained_projection());
  EXPECT_EQ(member(bundle->normalized_root(), "assistant"),
            qualified_assistant.normalized_projection());
  EXPECT_EQ(canonical_json(member(bundle->normalized_root(), "assistant")),
            bundle->value().assistant->canonical_json);
  EXPECT_EQ(
      array_member(bundle->normalized_root(), "external_resources").size(), 2U);

  const auto reloaded =
      detail::RetainedBundleReloadAuthority::reload(bundle->canonical_bytes());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->retained_bundle_id(), bundle->retained_bundle_id());
  EXPECT_EQ(reloaded->value(), bundle->value());
  EXPECT_EQ(reloaded->canonical_bytes(), bundle->canonical_bytes());

  test::InMemoryCatalogArtifactStoreFixture second_catalog_store;
  test::InMemoryRawArtifactStoreFixture second_raw_store;
  const auto regenerated = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), second_catalog_store,
      second_raw_store);
  ASSERT_NE(regenerated, nullptr);
  EXPECT_EQ(regenerated->retained_bundle_id(), bundle->retained_bundle_id());
  EXPECT_EQ(regenerated->canonical_bytes(), bundle->canonical_bytes());

  CanonicalValue changed_assistant =
      member(bundle->normalized_root(), "assistant");
  CanonicalValue::Array modes = array_member(changed_assistant, "modes");
  ASSERT_EQ(modes.size(), 1U);
  modes.front() = replace_object_member(modes.front(), "description",
                                        CanonicalValue("changed description"));
  changed_assistant = replace_object_member(changed_assistant, "modes",
                                            CanonicalValue(std::move(modes)));
  CanonicalValue changed_root = replace_root_member(
      bundle->normalized_root(), "assistant", std::move(changed_assistant));
  changed_root = reidentify_retained_bundle(std::move(changed_root));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(changed_root))),
               RetainedBundleError);

  // Assistant aliases no longer resolve against external_resources (batch I:
  // models are served by the AI model component), so an external resource
  // kind change is not an assistant requalification failure.
}

TEST(
    ControlDomainControlVerifier,
    PneumaticFloorRequiresExactSourceAndVerifiedModelCoordinatesAfterMaterialization) {
  for (const std::string_view kind : {"position_commanded", "pneumatic"}) {
    for (const std::string_view mode : {"commanded", "none"}) {
      if (kind == "position_commanded" && mode == "none")
        continue;
      const auto fixture =
          test::make_lite_gripper_only_retention_fixture({}, kind, 0.25, mode);
      const test::Lite6ControllableCompilerInputArtifactSource input_source(
          fixture);
      test::InMemoryCatalogArtifactStoreFixture catalog_store;
      test::InMemoryRawArtifactStoreFixture raw_store;
      const auto bundle = detail::RetainedBundleMaterializer::materialize(
          fixture.qualified,
          detail::RetentionMaterializerTestAccess::make_context(), input_source,
          fixture.structural_preparations(), catalog_store, raw_store);
      ASSERT_NE(bundle, nullptr);
      ASSERT_TRUE(verify_retained_bundle_identity(*bundle));
      const auto &graph = bundle->value().cell_graph;
      const auto &coordinates = fixture.qualified.compiler_ir()
                                    .applicability()
                                    .composition()
                                    .control_binding_coordinate_maps();
      ASSERT_EQ(
          bundle->value().compiled_products.control_domain_products.size(), 1U);
      const auto &group =
          bundle->value().compiled_products.control_domain_products.front();
      ASSERT_EQ(group.products.size(), 1U);
      const std::string bytes = require_product_artifact_bytes(
          group.products.front(), detail::kControlDomainControlArtifactRole,
          raw_store);
      const auto control = detail::parse_and_verify_control_domain_control(
          bytes, &graph, coordinates);
      EXPECT_EQ(canonical_json(control), bytes);
      // The actuation kind is source context, not a new member in the retained control wire.
      EXPECT_EQ(control.find_object_member("gripper_actuation_kind"), nullptr);
      EXPECT_NO_THROW(
          detail::require_control_domain_control_matches_retained_bundle(
              bytes, bundle->value(), group.control_domain_id, {}, {}, {},
              coordinates));
      if (kind == "position_commanded") {
        EXPECT_NO_THROW(
            (void)detail::parse_and_verify_control_domain_control(bytes));
        continue; // Existing physical/default parser semantics remain exact.
      }
      EXPECT_THROW((void)detail::parse_and_verify_control_domain_control(bytes),
                   RetainedBundleError);
      EXPECT_THROW(
          (void)detail::parse_and_verify_control_domain_control(bytes, &graph),
          RetainedBundleError);
      auto missing_coordinates = coordinates;
      for (auto &binding : missing_coordinates)
        for (auto &resource : binding.resources)
          resource.coordinates.clear();
      EXPECT_THROW((void)detail::parse_and_verify_control_domain_control(
                       bytes, &graph, missing_coordinates),
                   RetainedBundleError);
      auto foreign_coordinates = coordinates;
      ASSERT_FALSE(foreign_coordinates.empty());
      ASSERT_FALSE(foreign_coordinates.front().resources.empty());
      foreign_coordinates.front().resources.front().resource_id =
          core::ResourceId("foreign.resource");
      EXPECT_THROW((void)detail::parse_and_verify_control_domain_control(
                       bytes, &graph, foreign_coordinates),
                   RetainedBundleError);
      auto foreign_model = coordinates;
      ASSERT_FALSE(foreign_model.front().resources.front().coordinates.empty());
      foreign_model.front()
          .resources.front()
          .coordinates.front()
          .model_instance_id = core::ModelInstanceId("foreign.model");
      EXPECT_THROW((void)detail::parse_and_verify_control_domain_control(
                       bytes, &graph, foreign_model),
                   RetainedBundleError);
      auto wrong_domain = graph;
      ASSERT_EQ(wrong_domain.control_domains.size(), 1U);
      wrong_domain.control_domains.front().control_domain_revision =
          core::ControlDomainRevision("sha256:" + std::string(64U, '0'));
      EXPECT_THROW((void)detail::parse_and_verify_control_domain_control(
                       bytes, &wrong_domain, coordinates),
                   RetainedBundleError);
      auto foreign_provider = graph;
      foreign_provider.control_bindings.front().provider_runtime_unit_id =
          core::RuntimeUnitId("foreign.provider.unit");
      EXPECT_THROW((void)detail::parse_and_verify_control_domain_control(
                       bytes, &foreign_provider, coordinates),
                   RetainedBundleError);
      auto missing_config = graph;
      ASSERT_EQ(missing_config.runtime_units.size(), 1U);
      missing_config.runtime_units.front()
          .configuration["gripper_target_configurations"] =
          core::JsonValueDTO{core::JsonValueDTO::Array{}};
      EXPECT_THROW((void)detail::parse_and_verify_control_domain_control(
                       bytes, &missing_config, coordinates),
                   RetainedBundleError);
      auto position_source = graph;
      auto &rows = std::get<core::JsonValueDTO::Array>(
          position_source.runtime_units.front()
              .configuration.at("gripper_target_configurations")
              .value);
      ASSERT_EQ(rows.size(), 1U);
      auto &row = std::get<core::JsonObjectDTO>(rows.front().value);
      row.at("actuation_kind") =
          core::JsonValueDTO{std::string("position_commanded")};
      row.at("output_mode") = core::JsonValueDTO{std::string("commanded")};
      EXPECT_THROW((void)detail::parse_and_verify_control_domain_control(
                       bytes, &position_source, coordinates),
                   RetainedBundleError);
    }
  }
}

TEST(ControlDomainControlVerifier,
     StoppableRowsRoundTripCloseAndRejectScopeTampering) {
  const auto fixture =
      test::make_lite6_stoppable_controllable_retention_fixture();
  const core::ControlDomainId domain_id(std::string(test::kLite6ArmDomainId));
  const std::string bytes = detail::emit_control_domain_control(
      fixture.qualified.compiler_ir(), domain_id);
  const CanonicalValue control =
      detail::parse_and_verify_control_domain_control(bytes);
  EXPECT_NO_THROW(detail::require_control_domain_control_matches(
      bytes, fixture.qualified.compiler_ir(), domain_id));
  const auto not_stoppable_fixture =
      test::make_lite6_controllable_retention_fixture();
  const std::string not_stoppable_bytes = detail::emit_control_domain_control(
      not_stoppable_fixture.qualified.compiler_ir(), domain_id);
  EXPECT_THROW(
      detail::require_control_domain_control_matches(
          not_stoppable_bytes, fixture.qualified.compiler_ir(), domain_id),
      RetainedBundleError);

  const auto &stops = array_member(control, "stop_applicability");
  ASSERT_EQ(stops.size(), 1U);
  const CanonicalValue &stop = stops.front();
  EXPECT_EQ(string_member(stop, "disposition"), "stoppable");
  EXPECT_EQ(string_member(stop, "stop_evidence_policy_id"),
            "lite6_controller_goal_terminal_stop");
  // The retained Stop policy carries no shared cell Stop budget.
  EXPECT_FALSE(std::get<CanonicalValue::Object>(stop.storage()).end() !=
               std::find_if(std::get<CanonicalValue::Object>(stop.storage()).begin(),
                            std::get<CanonicalValue::Object>(stop.storage()).end(),
                            [](const auto &row) { return row.first == "deadline_budget"; }));

  const auto &stages = array_member(stop, "escalation_stages");
  ASSERT_EQ(stages.size(), 2U);
  const CanonicalValue &nominal = stages.front();
  EXPECT_EQ(string_member(nominal, "stop_escalation_stage_id"), "nominal");
  EXPECT_EQ(string_member(nominal, "stage_kind"), "nominal");
  const CanonicalValue &command = member(nominal, "containment_command");
  EXPECT_EQ(string_member(command, "command_kind"), "controller_goal_cancel");
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      member(command, "provider_contract_ref").storage()));
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      member(command, "registered_command_kind_id").storage()));
  const auto &evidence = array_member(nominal, "all_of_evidence_requirements");
  ASSERT_EQ(evidence.size(), 1U);
  // The ROS arm without a qualified controlled Stop trusts its controller:
  // the nominal stage ends on the goal's terminal status.
  EXPECT_EQ(string_member(evidence.front(), "evidence_kind"),
            "controller_goal_terminal");
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      member(evidence.front(), "provider_contract_ref").storage()));
  EXPECT_EQ(string_member(member(evidence.front(), "source_requirement"),
                          "source_kind"),
            "controller_goal_status");
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      member(member(evidence.front(), "source_requirement"), "endpoint_role_id")
          .storage()));
  EXPECT_TRUE(array_member(nominal, "provider_evidence_endpoints").empty());
  EXPECT_TRUE(array_member(nominal, "binding_state_authority_sources").empty());
  const auto &scopes = array_member(nominal, "containment_scopes");
  ASSERT_EQ(scopes.size(), 1U);
  EXPECT_EQ(string_member(scopes.front(), "scope_kind"), "resource");
  ASSERT_EQ(array_member(scopes.front(), "resource_ids").size(), 6U);

  const CanonicalValue &software_hard = stages.back();
  EXPECT_EQ(string_member(software_hard, "stop_escalation_stage_id"),
            "software_hard");
  EXPECT_EQ(string_member(software_hard, "stage_kind"), "software_hard");
  const CanonicalValue &hard_command =
      member(software_hard, "containment_command");
  EXPECT_EQ(string_member(hard_command, "command_kind"), "registered");
  EXPECT_EQ(string_member(hard_command, "registered_command_kind_id"),
            "manyforge.stop.software_hard");
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      member(software_hard, "provider_command_endpoint").storage()));

  CanonicalValue::Array open_rows = stops;
  CanonicalValue::Object open_stop = open_rows.front().as_object();
  open_stop.emplace_back("unqualified_extension", CanonicalValue(true));
  open_rows.front() = CanonicalValue(std::move(open_stop));
  const CanonicalValue open_control = replace_object_member(
      control, "stop_applicability", CanonicalValue(std::move(open_rows)));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_control_domain_control(
          canonical_json(open_control))),
      RetainedBundleError);

  CanonicalValue::Array tampered_rows = stops;
  CanonicalValue::Array tampered_stages = stages;
  CanonicalValue::Array tampered_scopes = scopes;
  tampered_scopes.front() =
      replace_object_member(tampered_scopes.front(), "resource_ids",
                            CanonicalValue(CanonicalValue::Array{
                                CanonicalValue("foreign_resource"),
                            }));
  tampered_stages.front() =
      replace_object_member(tampered_stages.front(), "containment_scopes",
                            CanonicalValue(std::move(tampered_scopes)));
  tampered_rows.front() =
      replace_object_member(tampered_rows.front(), "escalation_stages",
                            CanonicalValue(std::move(tampered_stages)));
  const CanonicalValue tampered_control = replace_object_member(
      control, "stop_applicability", CanonicalValue(std::move(tampered_rows)));
  const std::string tampered_bytes = canonical_json(tampered_control);
  EXPECT_THROW(
      static_cast<void>(
          detail::parse_and_verify_control_domain_control(tampered_bytes)),
      RetainedBundleError);
}

 TEST(ControlDomainControlVerifier,
     SelfConsistentImplementationRelabelStillRejects) {
  const auto direct = test::make_lite6_controllable_retention_fixture();
  const auto chained =
      test::make_lite6_chained_controllable_retention_fixture();
  const core::ControlDomainId domain_id(std::string(test::kLite6ArmDomainId));
  CanonicalValue direct_control =
      detail::parse_and_verify_control_domain_control(
          detail::emit_control_domain_control(direct.qualified.compiler_ir(),
                                              domain_id));
  const CanonicalValue chained_control =
      detail::parse_and_verify_control_domain_control(
          detail::emit_control_domain_control(chained.qualified.compiler_ir(),
                                              domain_id));

  CanonicalValue::Array direct_controllers =
      array_member(direct_control, "controllers");
  ASSERT_EQ(direct_controllers.size(), 1U);
  const auto &chained_controllers =
      array_member(chained_control, "controllers");
  ASSERT_EQ(chained_controllers.size(), 2U);
  ASSERT_EQ(string_member(chained_controllers[1], "controller_role_id"),
            "trajectory");
  direct_controllers.front() = recompute_controller_configuration_digest(
      replace_object_member(direct_controllers.front(), "implementation",
                            member(chained_controllers[1], "implementation")));
  direct_control =
      replace_object_member(std::move(direct_control), "controllers",
                            CanonicalValue(std::move(direct_controllers)));
  direct_control =
      recompute_control_source_conformance(std::move(direct_control));

  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_control_domain_control(
          canonical_json(direct_control))),
      RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     ChainedLite6RetainsOrderedJtcAdmittanceControllerProduct) {
  const auto fixture =
      test::make_lite6_chained_controllable_retention_fixture();
  // The execution host admission subleg retires; the chained CCT remains a
  // normal metadata/compiler/emitter path. Durable closure uses a real ROS host.
  const auto ros_fixture =
      test::make_lite6_ros2_control_shared_consumer_retention_fixture();
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      ros_fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      ros_fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      ros_fixture.structural_preparations(), catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  ASSERT_EQ(bundle->value().compiled_products.control_domain_products.size(),
            1U);
  const auto &product = bundle->value()
                            .compiled_products.control_domain_products.front()
                            .products.front();
  const std::string retained_control_bytes = require_product_artifact_bytes(
      product, detail::kControlDomainControlArtifactRole, raw_store);
  EXPECT_NO_THROW(detail::require_control_domain_control_matches(
      retained_control_bytes, ros_fixture.qualified.compiler_ir(),
      core::ControlDomainId(std::string(test::kLite6ArmDomainId))));
  const std::string bytes = detail::emit_control_domain_control(
      fixture.qualified.compiler_ir(),
      core::ControlDomainId(std::string(test::kLite6ArmDomainId)));
  const CanonicalValue control =
      detail::parse_and_verify_control_domain_control(bytes);
  EXPECT_NO_THROW(detail::require_control_domain_control_matches(
      bytes, fixture.qualified.compiler_ir(),
      core::ControlDomainId(std::string(test::kLite6ArmDomainId))));

  const auto &controllers = array_member(control, "controllers");
  ASSERT_EQ(controllers.size(), 2U);
  EXPECT_EQ(string_member(controllers[0], "controller_role_id"), "admittance");
  EXPECT_EQ(string_member(member(controllers[0], "implementation"),
                          "implementation_id"),
            "manyforge.ros2_control.admittance_controller");
  EXPECT_EQ(string_member(controllers[1], "controller_role_id"), "trajectory");
  EXPECT_EQ(string_member(member(controllers[1], "implementation"),
                          "implementation_id"),
            "manyforge.ros2_control.joint_trajectory_controller.chainable");

  const auto &routes = array_member(control, "routes");
  ASSERT_EQ(routes.size(), 1U);
  const auto &ordered_roles =
      array_member(routes.front(), "controller_role_ids");
  ASSERT_EQ(ordered_roles.size(), 2U);
  EXPECT_EQ(std::get<std::string>(ordered_roles[0].storage()), "trajectory");
  EXPECT_EQ(std::get<std::string>(ordered_roles[1].storage()), "admittance");
  const auto &path_edges = array_member(routes.front(), "path_edges");
  ASSERT_EQ(path_edges.size(), 1U);
  EXPECT_EQ(array_member(path_edges.front(), "reference_interfaces").size(),
            6U);
  EXPECT_EQ(array_member(path_edges.front(), "state_interfaces").size(), 6U);
  EXPECT_EQ(string_member(member(control, "desired_configuration"),
                          "activation_set_id"),
            "arm_chain");

  test::InMemoryCatalogArtifactStoreFixture second_catalog_store;
  test::InMemoryRawArtifactStoreFixture second_raw_store;
  const auto regenerated = detail::RetainedBundleMaterializer::materialize(
      ros_fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      ros_fixture.structural_preparations(), second_catalog_store,
      second_raw_store);
  ASSERT_NE(regenerated, nullptr);
  EXPECT_EQ(regenerated->retained_bundle_id(), bundle->retained_bundle_id());
  EXPECT_EQ(regenerated->canonical_bytes(), bundle->canonical_bytes());
  EXPECT_EQ(require_product_artifact_bytes(
                regenerated->value()
                    .compiled_products.control_domain_products.front()
                    .products.front(),
                detail::kControlDomainControlArtifactRole, second_raw_store),
            retained_control_bytes);
  EXPECT_EQ(detail::emit_control_domain_control(
                fixture.qualified.compiler_ir(),
                core::ControlDomainId(std::string(test::kLite6ArmDomainId))),
            bytes);
}

TEST(RetainedBundleMaterializer,
     ControllableLite6SymbolMapRejectsOwnerTargetResourceAndMapSplice) {
  const auto fixture = test::make_lite6_controllable_retention_fixture();
  const std::string bytes = detail::emit_composed_model_symbol_role_map(
      fixture.qualified.compiler_ir());
  const CanonicalValue parsed =
      detail::parse_and_verify_composed_model_symbol_role_map(bytes);

  const CanonicalValue wrong_owner = replace_first_array_object_member(
      parsed, "model_occurrences", "owner_component_id",
      CanonicalValue("foreign_component"));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          canonical_json(wrong_owner))),
      RetainedBundleError);

  const CanonicalValue wrong_target = replace_first_array_object_member(
      parsed, "control_binding_coordinate_maps", "command_target_id",
      CanonicalValue("foreign_target"));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          canonical_json(wrong_target))),
      RetainedBundleError);

  CanonicalValue::Array binding_maps =
      array_member(parsed, "control_binding_coordinate_maps");
  CanonicalValue::Array resources =
      array_member(binding_maps.front(), "resources");
  ASSERT_EQ(resources.size(), 6U);
  resources.front() = replace_object_member(resources.front(), "resource_id",
                                            CanonicalValue("lite6_joint_0"));
  binding_maps.front() = replace_object_member(
      binding_maps.front(), "resources", CanonicalValue(std::move(resources)));
  const CanonicalValue wrong_resource =
      replace_root_member(parsed, "control_binding_coordinate_maps",
                          CanonicalValue(std::move(binding_maps)));
  const std::string wrong_resource_bytes = canonical_json(wrong_resource);
  EXPECT_NO_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          wrong_resource_bytes)));
  EXPECT_THROW(detail::require_composed_model_symbol_role_map_matches(
                   wrong_resource_bytes, fixture.qualified.compiler_ir()),
               RetainedBundleError);

  const auto passive_input = test::make_lite6_structural_retention_fixture();
  const auto passive = test::qualify_lite6_passive_deployment(passive_input);
  const std::string spliced =
      detail::emit_composed_model_symbol_role_map(passive.compiler_ir());
  EXPECT_NO_THROW(static_cast<void>(
      detail::parse_and_verify_composed_model_symbol_role_map(spliced)));
  EXPECT_THROW(detail::require_composed_model_symbol_role_map_matches(
                   spliced, fixture.qualified.compiler_ir()),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     ComposedSymbolRoleMapCodecIsCanonicalClosedAndBijective) {
  const auto fixture = test::make_lite6_structural_retention_fixture();
  const auto qualified = test::qualify_lite6_passive_deployment(fixture);
  const std::string bytes =
      detail::emit_composed_model_symbol_role_map(qualified.compiler_ir());
  const CanonicalValue parsed =
      detail::parse_and_verify_composed_model_symbol_role_map(bytes);
  EXPECT_EQ(canonical_json(parsed), bytes);
  EXPECT_NO_THROW(detail::require_composed_model_symbol_role_map_matches(
      bytes, qualified.compiler_ir()));

  EXPECT_THROW(
      static_cast<void>(
          detail::parse_and_verify_composed_model_symbol_role_map(" " + bytes)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          std::string(detail::kMaxComposedModelSymbolRoleMapBytes + 1U, 'x'))),
      RetainedBundleError);

  CanonicalValue::Object extra_fields = parsed.as_object();
  extra_fields.emplace_back("consumer_hint", CanonicalValue("roboplan"));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          canonical_json(CanonicalValue(std::move(extra_fields))))),
      RetainedBundleError);

  CanonicalValue::Array symbols = array_member(parsed, "symbol_rows");
  ASSERT_GT(symbols.size(), 1U);
  CanonicalValue::Object first = symbols[0].as_object();
  const auto compiled =
      std::find_if(first.begin(), first.end(), [](const auto &member) {
        return member.first == "compiled_name";
      });
  ASSERT_NE(compiled, first.end());
  compiled->second = CanonicalValue(string_member(symbols[1], "compiled_name"));
  symbols[0] = CanonicalValue(std::move(first));
  const CanonicalValue duplicate_reverse = replace_root_member(
      parsed, "symbol_rows", CanonicalValue(std::move(symbols)));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          canonical_json(duplicate_reverse))),
      RetainedBundleError);

  CanonicalValue::Array out_of_range_paths =
      array_member(parsed, "frame_paths");
  const auto path_with_fixed = std::find_if(
      out_of_range_paths.begin(), out_of_range_paths.end(),
      [](const CanonicalValue &path) {
        const auto &segments = array_member(path, "segments");
        return !segments.empty() &&
               string_member(segments.front(), "kind") == "fixed_connection";
      });
  ASSERT_NE(path_with_fixed, out_of_range_paths.end());
  CanonicalValue::Array out_of_range_segments =
      array_member(*path_with_fixed, "segments");
  out_of_range_segments.front() = replace_object_member(
      out_of_range_segments.front(), "fixed_connection_index",
      CanonicalValue(9007199254740991.0));
  *path_with_fixed =
      replace_object_member(*path_with_fixed, "segments",
                            CanonicalValue(std::move(out_of_range_segments)));
  const CanonicalValue out_of_range_path = replace_root_member(
      parsed, "frame_paths", CanonicalValue(std::move(out_of_range_paths)));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          canonical_json(out_of_range_path))),
      RetainedBundleError);

  CanonicalValue::Array reordered_paths = array_member(parsed, "frame_paths");
  ASSERT_GT(reordered_paths.size(), 1U);
  std::swap(reordered_paths[0], reordered_paths[1]);
  const CanonicalValue reordered_path = replace_root_member(
      parsed, "frame_paths", CanonicalValue(std::move(reordered_paths)));
  EXPECT_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          canonical_json(reordered_path))),
      RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     PassiveLite6RequiresExactPreparationBijectionBeforeStoreEffects) {
  const auto fixture = test::make_lite6_structural_retention_fixture();
  const auto qualified = test::qualify_lite6_passive_deployment(fixture);
  const test::Lite6CompilerInputArtifactSource input_source(fixture);

  {
    test::InMemoryCatalogArtifactStoreFixture catalog_store;
    CountingRawArtifactStore raw_store;
    EXPECT_THROW(
        static_cast<void>(detail::RetainedBundleMaterializer::materialize(
            qualified, detail::RetentionMaterializerTestAccess::make_context(),
            input_source, {}, catalog_store, raw_store)),
        RetainedBundleError);
    EXPECT_EQ(catalog_store.size(), 0U);
    EXPECT_EQ(raw_store.put_calls(), 0U);
    EXPECT_EQ(raw_store.get_calls(), 0U);
  }

  {
    const std::array<BuildQualifiedStructuralModelPreparation, 2U>
        duplicate_preparations{fixture.preparation, fixture.preparation};
    test::InMemoryCatalogArtifactStoreFixture catalog_store;
    CountingRawArtifactStore raw_store;
    EXPECT_THROW(
        static_cast<void>(detail::RetainedBundleMaterializer::materialize(
            qualified, detail::RetentionMaterializerTestAccess::make_context(),
            input_source, duplicate_preparations, catalog_store, raw_store)),
        RetainedBundleError);
    EXPECT_EQ(catalog_store.size(), 0U);
    EXPECT_EQ(raw_store.put_calls(), 0U);
    EXPECT_EQ(raw_store.get_calls(), 0U);
  }
}

TEST(RetainedBundleMaterializer,
     PlanningResourceMapDeduplicatesOneExactAssetAcrossOccurrences) {
  const auto fixture = test::make_lite6_structural_retention_fixture();
  const std::array<test::Lite6PassiveModelSelection, 2U> selections{{
      {"lite6_a", &fixture},
      {"lite6_b", &fixture},
  }};
  const auto qualified = test::qualify_lite6_passive_deployment(selections);
  const test::Lite6CompilerInputArtifactSource input_source(fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      qualified, detail::RetentionMaterializerTestAccess::make_context(),
      input_source, fixture.structural_preparations(), catalog_store,
      raw_store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_EQ(bundle->value().catalogs.records.size(), 1U);
  EXPECT_EQ(bundle->value().provenance.source_records.size(), 1U);
  EXPECT_EQ(bundle->value().provenance.generator_records.size(), 1U);
  const auto rows = detail::parse_and_verify_planning_resource_map(
      require_product_artifact_bytes(
          *bundle, detail::kPlanningResourceMapArtifactRole, raw_store));
  const std::size_t expected_resources = fixture.source_artifacts.size() - 2U;
  ASSERT_EQ(rows.size(), expected_resources);
  const std::string urdf =
      require_product_artifact_bytes(*bundle, "urdf", raw_store);
  for (const auto &row : rows) {
    EXPECT_EQ(urdf.find(row.source_logical_uri), std::string::npos);
    const auto first = urdf.find(row.compiled_logical_uri);
    ASSERT_NE(first, std::string::npos);
    EXPECT_NE(urdf.find(row.compiled_logical_uri,
                        first + row.compiled_logical_uri.size()),
              std::string::npos);
  }
}

TEST(RetainedBundleMaterializer,
     PlanningResourceMapSeparatesMetadataOnlyRevisionsDeterministically) {
  const auto revision_a = test::make_lite6_structural_retention_fixture();
  const auto revision_b =
      test::make_lite6_structural_retention_fixture("arm_collision_revision_b");
  ASSERT_EQ(revision_a.catalog_reference.record_id,
            revision_b.catalog_reference.record_id);
  ASSERT_NE(revision_a.catalog_reference.record_revision,
            revision_b.catalog_reference.record_revision);
  ASSERT_EQ(revision_a.source_artifacts, revision_b.source_artifacts);
  ASSERT_EQ(revision_a.preparation.generator_record(),
            revision_b.preparation.generator_record());

  const std::array<test::Lite6PassiveModelSelection, 2U> selections_a{{
      {"lite6_a", &revision_a},
      {"lite6_b", &revision_b},
  }};
  const std::array<test::Lite6PassiveModelSelection, 2U> selections_b{{
      {"lite6_b", &revision_b},
      {"lite6_a", &revision_a},
  }};
  const auto qualified_a = test::qualify_lite6_passive_deployment(selections_a);
  const auto qualified_b = test::qualify_lite6_passive_deployment(selections_b);
  const test::Lite6CompilerInputArtifactSource input_a(
      {&revision_a, &revision_b});
  const test::Lite6CompilerInputArtifactSource input_b(
      {&revision_b, &revision_a});
  const std::array<BuildQualifiedStructuralModelPreparation, 2U> preparations_a{
      revision_a.preparation, revision_b.preparation};
  const std::array<BuildQualifiedStructuralModelPreparation, 2U> preparations_b{
      revision_b.preparation, revision_a.preparation};
  test::InMemoryCatalogArtifactStoreFixture catalogs_a;
  test::InMemoryCatalogArtifactStoreFixture catalogs_b;
  test::InMemoryRawArtifactStoreFixture raw_a;
  test::InMemoryRawArtifactStoreFixture raw_b;

  const auto bundle_a = detail::RetainedBundleMaterializer::materialize(
      qualified_a, detail::RetentionMaterializerTestAccess::make_context(),
      input_a, preparations_a, catalogs_a, raw_a);
  const auto bundle_b = detail::RetainedBundleMaterializer::materialize(
      qualified_b, detail::RetentionMaterializerTestAccess::make_context(),
      input_b, preparations_b, catalogs_b, raw_b);

  ASSERT_NE(bundle_a, nullptr);
  ASSERT_NE(bundle_b, nullptr);
  EXPECT_EQ(bundle_a->retained_bundle_id(), bundle_b->retained_bundle_id());
  EXPECT_EQ(bundle_a->canonical_bytes(), bundle_b->canonical_bytes());
  EXPECT_EQ(bundle_a->value().catalogs.records.size(), 2U);
  EXPECT_EQ(bundle_a->value().provenance.source_records.size(), 2U);
  EXPECT_EQ(bundle_a->value().provenance.generator_records.size(), 1U);

  const auto rows = detail::parse_and_verify_planning_resource_map(
      require_product_artifact_bytes(
          *bundle_a, detail::kPlanningResourceMapArtifactRole, raw_a));
  const std::size_t expected_per_revision =
      revision_a.source_artifacts.size() - 2U;
  ASSERT_EQ(rows.size(), expected_per_revision * 2U);
  for (std::size_t index = 0U; index < expected_per_revision; ++index) {
    const auto &first = rows[index];
    const auto &second = rows[index + expected_per_revision];
    EXPECT_EQ(first.source_logical_uri, second.source_logical_uri);
    EXPECT_EQ(first.content_digest, second.content_digest);
    EXPECT_EQ(first.content_addressed_store_key,
              second.content_addressed_store_key);
    EXPECT_NE(first.model_revision, second.model_revision);
    EXPECT_NE(first.compiled_logical_uri, second.compiled_logical_uri);
  }
}

TEST(RetainedBundleMaterializer,
     PlanningResourceMapCodecIsCanonicalClosedAndFailClosed) {
  const core::ModelAssetId asset_id("resource_fixture");
  const core::ModelRevision revision(
      "sha256:"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  const std::string source_uri = "asset://resource_fixture/meshes/base.stl";
  const std::string digest =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const detail::PlanningResourceMapRow row{
      asset_id,
      revision,
      source_uri,
      detail::compiled_planning_resource_uri(asset_id, revision, source_uri),
      "model/stl",
      12U,
      core::ContentDigest(digest),
      core::ContentAddressedStoreKey("sha256/" + digest.substr(7U)),
  };
  const std::string canonical = detail::emit_planning_resource_map({row});
  EXPECT_EQ(detail::parse_and_verify_planning_resource_map(canonical),
            std::vector<detail::PlanningResourceMapRow>{row});

  auto duplicate = std::vector<detail::PlanningResourceMapRow>{row, row};
  EXPECT_THROW(static_cast<void>(
                   detail::emit_planning_resource_map(std::move(duplicate))),
               RetainedBundleError);

  auto wrong_store = row;
  wrong_store.content_addressed_store_key = core::ContentAddressedStoreKey(
      "sha256/"
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
  EXPECT_THROW(
      static_cast<void>(detail::emit_planning_resource_map({wrong_store})),
      RetainedBundleError);

  std::string tampered = canonical;
  const auto compiled_offset = tampered.find(row.compiled_logical_uri);
  ASSERT_NE(compiled_offset, std::string::npos);
  tampered.insert(compiled_offset + row.compiled_logical_uri.size(), "x");
  EXPECT_THROW(static_cast<void>(
                   detail::parse_and_verify_planning_resource_map(tampered)),
               RetainedBundleError);
  EXPECT_THROW(static_cast<void>(detail::parse_and_verify_planning_resource_map(
                   canonical + "\n")),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     PinocchioFamilyProfileAcceptsOneLevelMimicAndRejectsChain) {
  UrdfDocumentIr one_level;
  one_level.robot_name = "one_level";
  UrdfJointIr master{};
  master.name = "master";
  UrdfJointIr follower{};
  follower.name = "follower";
  follower.mimic = UrdfMimicIr{
      .joint_name = "master",
      .authored_multiplier = 1.0,
      .authored_offset = 0.0,
      .multiplier = 1.0,
      .offset = 0.0,
  };
  one_level.joints = {master, follower};
  EXPECT_NO_THROW(detail::require_pinocchio_family_mimic_profile(one_level));

  UrdfDocumentIr chain = one_level;
  UrdfJointIr second_follower{};
  second_follower.name = "second_follower";
  second_follower.mimic = UrdfMimicIr{
      .joint_name = "follower",
      .authored_multiplier = 2.0,
      .authored_offset = 0.5,
      .multiplier = 2.0,
      .offset = 0.5,
  };
  chain.joints.push_back(second_follower);
  EXPECT_THROW(detail::require_pinocchio_family_mimic_profile(chain),
               RetainedBundleError);

  std::reverse(chain.joints.begin(), chain.joints.end());
  EXPECT_THROW(detail::require_pinocchio_family_mimic_profile(chain),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     RegenerationIsByteExactAndRepositoryRoundTripReverifiesWholeEnvelope) {
  test::InMemoryRawArtifactStoreFixture first_store;
  test::InMemoryRawArtifactStoreFixture second_store;
  const auto first = materialize_empty("empty_cell", first_store);
  const auto second = materialize_empty("empty_cell", second_store);

  EXPECT_EQ(first->retained_bundle_id(), second->retained_bundle_id());
  EXPECT_EQ(first->normalized_root(), second->normalized_root());
  EXPECT_EQ(first->canonical_bytes(), second->canonical_bytes());

  test::InMemoryRetainedBundleRepositoryFixture repository;
  EXPECT_TRUE(repository.put_if_absent(first));
  EXPECT_FALSE(repository.put_if_absent(second));
  EXPECT_EQ(repository.size(), 1U);
  EXPECT_EQ(repository.retained_bytes(), first->canonical_bytes().size());

  const auto loaded =
      repository.get_verified_envelope(first->retained_bundle_id());
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*loaded));
  EXPECT_EQ(loaded->retained_bundle_id(), first->retained_bundle_id());
  EXPECT_EQ(loaded->value(), first->value());
  EXPECT_EQ(loaded->normalized_root(), first->normalized_root());
  EXPECT_EQ(loaded->canonical_bytes(), first->canonical_bytes());
}

TEST(RetainedBundleMaterializer,
     SealedRepositoryEnforcesActualBundleCountAndByteBounds) {
  test::InMemoryRawArtifactStoreFixture first_store;
  test::InMemoryRawArtifactStoreFixture second_store;
  const auto first = materialize_empty("bound_cell_a", first_store);
  const auto second = materialize_empty("bound_cell_b", second_store);

  ASSERT_GT(first->canonical_bytes().size(), 1U);
  test::InMemoryRetainedBundleRepositoryFixture byte_bounded(
      2U, first->canonical_bytes().size() - 1U);
  EXPECT_THROW(static_cast<void>(byte_bounded.put_if_absent(first)),
               RetainedBundleError);
  EXPECT_EQ(byte_bounded.size(), 0U);
  EXPECT_EQ(byte_bounded.retained_bytes(), 0U);

  const std::uint64_t combined_bytes =
      static_cast<std::uint64_t>(first->canonical_bytes().size()) +
      static_cast<std::uint64_t>(second->canonical_bytes().size());
  test::InMemoryRetainedBundleRepositoryFixture count_bounded(1U,
                                                              combined_bytes);
  EXPECT_TRUE(count_bounded.put_if_absent(first));
  EXPECT_THROW(static_cast<void>(count_bounded.put_if_absent(second)),
               RetainedBundleError);
  EXPECT_EQ(count_bounded.size(), 1U);
  EXPECT_EQ(count_bounded.retained_bytes(), first->canonical_bytes().size());
}

TEST(RetainedBundleMaterializer,
     ValidCellIdentityChangesRemainOrdinaryCellSpecificProducts) {
  test::InMemoryRawArtifactStoreFixture first_store;
  test::InMemoryRawArtifactStoreFixture second_store;
  const auto first = materialize_empty("empty_cell_a", first_store);
  const auto second = materialize_empty("empty_cell_b", second_store);

  EXPECT_NE(first->retained_bundle_id(), second->retained_bundle_id());
  EXPECT_NE(first->canonical_bytes(), second->canonical_bytes());
  EXPECT_NE(
      string_member(member(first->normalized_root(), "cell_graph"), "cell_id"),
      string_member(member(second->normalized_root(), "cell_graph"),
                    "cell_id"));
  const auto &first_artifacts =
      first->value().compiled_products.planning_collision_model.artifact_refs;
  const auto &second_artifacts =
      second->value().compiled_products.planning_collision_model.artifact_refs;
  ASSERT_EQ(first_artifacts.size(), 3U);
  ASSERT_EQ(second_artifacts.size(), first_artifacts.size());
  for (std::size_t index = 0U; index < first_artifacts.size(); ++index) {
    EXPECT_EQ(first_artifacts[index].artifact_role_id,
              second_artifacts[index].artifact_role_id);
    if (first_artifacts[index].artifact_role_id.value() ==
        detail::kPlanningResourceMapArtifactRole) {
      EXPECT_EQ(first_artifacts[index].content_digest,
                second_artifacts[index].content_digest);
    } else {
      EXPECT_NE(first_artifacts[index].content_digest,
                second_artifacts[index].content_digest);
    }
  }
}

TEST(RetainedBundleMaterializer, ScenePosePolicySelectionSurvivesSealedReload) {
  test::InMemoryRawArtifactStoreFixture store;
  const std::vector<core::ScenePoseSourcePolicyDTO> policies{
      {"camera", "part",
       core::ScenePoseDetection3dKeyDTO{"part", "", 0.5, 0.0025, "first"},
       "continuous_unattached"}};
  const auto ordinary = materialize_empty("pose_policy_cell", store, 1.0);
  const auto selected = materialize_empty("pose_policy_cell", store, 1.0,
                                          std::nullopt, policies);
  EXPECT_EQ(selected->value().scene_pose_sources, policies);
  EXPECT_NE(selected->retained_bundle_id(), ordinary->retained_bundle_id());
  const auto reloaded = detail::RetainedBundleReloadAuthority::reload(
      selected->canonical_bytes());
  EXPECT_EQ(reloaded->value().scene_pose_sources, policies);
  EXPECT_EQ(reloaded->value(), selected->value());
  const auto tampered = replace_root_member(
      selected->normalized_root(), "scene_pose_sources",
      EncodeScenePoseSources(
          {{"camera", "part",
            core::ScenePoseDetection3dKeyDTO{"part", "", 0.5, 0.0025, "first"},
            "continuous"}}));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(tampered))),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     ComponentStreamOverridesAreVerifiedAndExposedToTheHost) {
  const auto fixture = test::make_lite6_ros2_control_motion_retention_fixture();
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);
  ASSERT_NE(bundle, nullptr);
  // A Deployment without overrides retains no member and exposes none.
  EXPECT_EQ(bundle->normalized_root().find_object_member("component_streams"),
            nullptr);
  EXPECT_TRUE(ComponentStreamParameterOverrides(*bundle).empty());

  ASSERT_FALSE(bundle->value().cell_graph.controllable_components.empty());
  const auto component = bundle->value()
                             .cell_graph.controllable_components.front()
                             .controllable_component_id;
  const std::vector<core::StreamParameterOverrideDTO> rows{
      {component, "map_updates", std::int64_t{5000000000}, std::nullopt,
       std::nullopt},
      {component, "measured_joints", std::int64_t{300000000}, std::nullopt,
       std::nullopt},
  };
  const auto with_streams = [&](CanonicalValue streams) {
    CanonicalValue::Object root = bundle->normalized_root().as_object();
    root.emplace_back("component_streams", std::move(streams));
    return canonical_json(
        reidentify_retained_bundle(CanonicalValue(std::move(root))));
  };
  const auto reloaded = detail::RetainedBundleReloadAuthority::reload(
      with_streams(encode_component_stream_overrides(rows)));
  ASSERT_NE(reloaded, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*reloaded));
  EXPECT_EQ(ComponentStreamParameterOverrides(*reloaded), rows);

  const std::vector<core::StreamParameterOverrideDTO> unknown{
      {core::ControllableComponentId("missing_component"), "map_updates",
       std::int64_t{5000000000}, std::nullopt, std::nullopt}};
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   with_streams(encode_component_stream_overrides(unknown)))),
               RetainedBundleError);
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   with_streams(CanonicalValue(CanonicalValue::Array{})))),
               RetainedBundleError);
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   with_streams(encode_component_stream_overrides(
                       {rows[1], rows[0]})))),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     MovingTakeoverBudgetIsOptionalRetainedAndBoundByTheBundleIdentity) {
  test::InMemoryRawArtifactStoreFixture store;
  const auto ordinary = materialize_empty("budget_cell", store);
  EXPECT_FALSE(
      ordinary->value().motion_takeover_preparation_budget_s.has_value());
  EXPECT_EQ(ordinary->normalized_root().find_object_member(
                "motion_takeover_preparation_budget_s"),
            nullptr);
  const auto moving = materialize_empty("budget_cell", store, 3.5);
  EXPECT_EQ(moving->value().motion_takeover_preparation_budget_s,
            std::optional<double>(3.5));
  EXPECT_NE(moving->retained_bundle_id(), ordinary->retained_bundle_id());
  const auto reloaded =
      detail::RetainedBundleReloadAuthority::reload(moving->canonical_bytes());
  EXPECT_EQ(reloaded->value(), moving->value());
  EXPECT_TRUE(verify_retained_bundle_identity(*reloaded));

  const auto tampered = replace_root_member(
      moving->normalized_root(), "motion_takeover_preparation_budget_s",
      CanonicalValue(7.0));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(tampered))),
               RetainedBundleError);
  const auto resealed = detail::RetainedBundleReloadAuthority::reload(
      canonical_json(reidentify_retained_bundle(tampered)));
  EXPECT_EQ(resealed->value().motion_takeover_preparation_budget_s,
            std::optional<double>(7.0));
  EXPECT_NE(resealed->retained_bundle_id(), moving->retained_bundle_id());
}

TEST(RetainedBundleMaterializer,
     MovingTakeoverBudgetRejectsInvalidPresentValuesEvenWithFreshIdentity) {
  test::InMemoryRawArtifactStoreFixture store;
  const auto moving = materialize_empty("budget_invalid_cell", store, 3.5);
  for (const auto &invalid : std::vector<CanonicalValue>{
           CanonicalValue(nullptr), CanonicalValue(false),
           CanonicalValue("3.5"), CanonicalValue(0.0), CanonicalValue(-1.0)}) {
    const auto changed = reidentify_retained_bundle(
        replace_root_member(moving->normalized_root(),
                            "motion_takeover_preparation_budget_s", invalid));
    EXPECT_THROW(
        static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
            canonical_json(changed))),
        RetainedBundleError);
  }
}

TEST(RetainedBundleMaterializer,
     MovingPlanningBudgetIsSealedAndValidatedAgainstTheWholeBudget) {
  test::InMemoryRawArtifactStoreFixture store;
  const auto legacy = materialize_empty("planning_budget_cell", store, 3.5);
  const auto moving =
      materialize_empty("planning_budget_cell", store, 3.5, 1.25);
  EXPECT_FALSE(legacy->value().motion_takeover_planning_budget_s);
  EXPECT_EQ(legacy->normalized_root().find_object_member(
                "motion_takeover_planning_budget_s"),
            nullptr);
  EXPECT_EQ(moving->value().motion_takeover_planning_budget_s,
            std::optional<double>(1.25));
  EXPECT_NE(moving->retained_bundle_id(), legacy->retained_bundle_id());
  const auto loaded =
      detail::RetainedBundleReloadAuthority::reload(moving->canonical_bytes());
  EXPECT_EQ(loaded->value(), moving->value());
  for (const auto &invalid : std::vector<CanonicalValue>{
           CanonicalValue(nullptr), CanonicalValue(false), CanonicalValue("1"),
           CanonicalValue(0.0), CanonicalValue(-1.0), CanonicalValue(3.5),
           CanonicalValue(4.0)}) {
    const auto changed = reidentify_retained_bundle(
        replace_root_member(moving->normalized_root(),
                            "motion_takeover_planning_budget_s", invalid));
    EXPECT_THROW(
        static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
            canonical_json(changed))),
        RetainedBundleError);
  }
  // A freshly sealed planning-only payload cannot bypass the authored boundary.
  const auto empty = materialize_empty("planning_only_cell", store);
  auto fields = empty->normalized_root().as_object();
  fields.emplace_back("motion_takeover_planning_budget_s", CanonicalValue(1.0));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(reidentify_retained_bundle(
                       CanonicalValue(std::move(fields)))))),
               RetainedBundleError);
}

} // namespace
} // namespace manyforge::deployment
