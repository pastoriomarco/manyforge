#include <gtest/gtest.h>

#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/contact_policy_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_contract_capability_declarations.hpp>
#include <manyforge_deployment/runtime_policy_codec.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
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
#include "lite6_controllable_retention_fixture.hpp"
#include "lite6_structural_retention_fixture.hpp"
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
            "manyforge_i3_materializer_test",
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
         "\"deployment_artifact_id\":\"i3_canonical_empty\","
         "\"deployment_revision\":\"" +
         std::string(kZeroDigest) + "\",\"cell_id\":\"" + std::string(cell_id) +
         "\",\"name\":\"I3 canonical empty\","
         "\"catalogs\":{\"records\":[]},"
         "\"graph\":" +
         empty_graph() +
         ",\"desired_control_configurations\":[],"
         "\"capability_provider_bindings\":[]}";
}

[[nodiscard]] RetentionQualifiedDeployment
qualify_empty(std::string_view cell_id) {
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(deployment_document(cell_id)),
      std::span<const QualifiedRuntimeProfile>{});
  const auto composition = compose_cell_occurrences(
      deployment.graph(), std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto selected_overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources, nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {}, {},
      {}, {}, {}, {}, selected_overlay);
  return qualify_deployment_for_retention(deployment,
                                          compile_cell_ir(applicability));
}

[[nodiscard]] std::shared_ptr<const RetainedBundle>
materialize_empty(std::string_view cell_id, IRawArtifactStore &store) {
  const auto qualified = qualify_empty(cell_id);
  const detail::RejectingCompilerInputArtifactSource input_source;
  detail::RejectingCatalogArtifactStore catalog_store;
  return detail::RetainedBundleMaterializer::materialize(
      qualified, detail::RetentionMaterializerTestAccess::make_context(),
      input_source, {}, catalog_store, store);
}

[[nodiscard]] CanonicalValue replace_object_member(const CanonicalValue &value,
                                                   std::string_view member_name,
                                                   CanonicalValue replacement);

struct RuntimePolicyRetentionFixture final {
  RetentionQualifiedDeployment qualified;
  QualifiedRuntimePolicy policy;
  std::string policy_bytes;
};

[[nodiscard]] RuntimePolicyRetentionFixture
make_runtime_policy_retention_fixture() {
  CanonicalValue policy(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.runtime_policy.v1")},
      {"runtime_policy_id", CanonicalValue("runtime.safe")},
      {"runtime_policy_revision", CanonicalValue(std::string(kZeroDigest))},
      {"interrupt_policy_ids", CanonicalValue(CanonicalValue::Array{})},
      {"motion_speed_limits",
       CanonicalValue(
           CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
               {"scope_kind", CanonicalValue("cell")},
               {"maximum_tcp_linear_speed_mps", CanonicalValue(0.5)},
           })})},
      {"open_loop_grasp_acknowledgements",
       CanonicalValue(CanonicalValue::Array{})},
  });
  policy = replace_object_member(
      policy, "runtime_policy_revision",
      CanonicalValue(detail::compute_content_identity(
          policy,
          detail::ContentIdentitySpec{
              .identity_domain = "catalog-record-revision/runtime_policy",
              .api_version = "manyforge.runtime_policy.v1",
              .own_field = "runtime_policy_revision",
          })));
  const std::string policy_bytes = canonical_json(policy);
  const std::string policy_digest = sha256_digest(policy_bytes);
  const core::RuntimePolicyCatalogRecordRefDTO reference{
      core::RuntimePolicyId("runtime.safe"),
      core::RuntimePolicyRevision(
          string_member(policy, "runtime_policy_revision")),
      core::SchemaId("manyforge.runtime_policy.v1"),
      core::SchemaRevision(
          "sha256:"
          "330a7505c585d69b231a1322bffbcd71775a1070c3a519a1ef7eb013ed5b5e9f"),
      core::ContentDigest(policy_digest),
      core::ContentAddressedStoreKey("sha256/" + policy_digest.substr(7U)),
  };
  QualifiedRuntimePolicy qualified_policy = resolve_and_qualify_runtime_policy(
      reference, reference.content_addressed_store_key, policy_bytes);

  const CanonicalValue catalogs = detail::encode_deployment_catalogs(
      std::array<core::CatalogRecordRefDTO, 1U>{reference});
  const CanonicalValue &records = member(catalogs, "records");
  const CanonicalValue deployment_root(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id", CanonicalValue("runtime_policy_retention")},
      {"deployment_revision", CanonicalValue(std::string(kZeroDigest))},
      {"cell_id", CanonicalValue("runtime_policy_cell")},
      {"name", CanonicalValue("Runtime policy retention")},
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
           {"interrupt_sources", CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_policies", CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_config",
            CanonicalValue(CanonicalValue::Object{
                {"queue_depth", CanonicalValue(8.0)},
                {"cancel_validation_timeout_ns", CanonicalValue(1000.0)},
                {"motion_scope_arm_timeout_ns", CanonicalValue(1000.0)},
                {"supervisory_poll_rate_hz", CanonicalValue(10.0)},
            })},
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
      composition, deployment.applicability_inputs(), {}, policies, {}, {}, {},
      {}, {}, {}, {}, {}, overlay);
  return {qualify_deployment_for_retention(deployment,
                                           compile_cell_ir(applicability)),
          std::move(qualified_policy), policy_bytes};
}

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
                             {"component_role_id", CanonicalValue("robot")},
                             {"model_role_id", CanonicalValue("arm_model")},
                             {"collision_body_role_id",
                              CanonicalValue("tool_collision")},
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
          "79f101a9c8cc2acf8db6ecaffd3476b03723afd5a31cce2a503f7b8764b47912"),
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
           {"interrupt_sources", CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_policies", CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_config",
            CanonicalValue(CanonicalValue::Object{
                {"queue_depth", CanonicalValue(8.0)},
                {"cancel_validation_timeout_ns", CanonicalValue(1000.0)},
                {"motion_scope_arm_timeout_ns", CanonicalValue(1000.0)},
                {"supervisory_poll_rate_hz", CanonicalValue(10.0)},
            })},
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
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {}, {},
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
  const auto qualified = qualify_empty("i3_empty_cell");
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
            "i3_canonical_empty");
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
     RuntimePolicySelectionIsRetainedAndReloadedAsOneExactCatalogReferent) {
  auto fixture = make_runtime_policy_retention_fixture();
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
  ASSERT_EQ(
      array_member(bundle->normalized_root(), "retained_policy_records").size(),
      1U);
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
  const auto bundle = materialize_empty("i3_viewer_splice_cell", store);
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
  const auto fixture = test::make_lite6_controllable_retention_fixture();
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
  EXPECT_EQ(bundle->value().catalogs.records.size(), 8U);
  EXPECT_EQ(bundle->value().catalogs.schema_artifacts.size(), 8U);
  EXPECT_EQ(bundle->value().provenance.source_records.size(), 8U);
  EXPECT_EQ(bundle->value().provenance.generator_records.size(), 1U);
  EXPECT_EQ(catalog_store.size(), 8U);
  ASSERT_EQ(bundle->value().desired_control_configurations.size(), 1U);
  EXPECT_TRUE(bundle->value().capability_provider_bindings.empty());
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
  const CanonicalValue control =
      detail::parse_and_verify_control_domain_control(control_bytes);
  EXPECT_NO_THROW(detail::require_control_domain_control_matches(
      control_bytes, fixture.qualified.compiler_ir(),
      core::ControlDomainId(std::string(test::kLite6ArmDomainId))));
  const auto chained_fixture =
      test::make_lite6_chained_controllable_retention_fixture();
  const std::string chained_bytes = detail::emit_control_domain_control(
      chained_fixture.qualified.compiler_ir(),
      core::ControlDomainId(std::string(test::kLite6ArmDomainId)));
  EXPECT_THROW(detail::require_control_domain_control_matches(
                   chained_bytes, fixture.qualified.compiler_ir(),
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
            test::kLite6NativeExecutionRuntimeId);
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
  const CanonicalValue parsed =
      detail::parse_and_verify_composed_model_symbol_role_map(symbol_map);
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
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(string_member(frames[0], "frame_role_id"), "base");
  EXPECT_EQ(string_member(frames[1], "frame_role_id"), "tcp");
  EXPECT_EQ(array_member(targets.front(), "collision_bodies").size(), 1U);
}

TEST(RetainedBundleMaterializer,
     NativeMotionLite6RetainsExactRuntimeProductAndProviderEndpoint) {
  const auto fixture = test::make_lite6_native_motion_retention_fixture();
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
  EXPECT_EQ(bundle->value().catalogs.records.size(), 13U);
  EXPECT_EQ(catalog_store.size(), 13U);
  ASSERT_EQ(bundle->value().cell_graph.runtime_units.size(), 1U);
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
            "lite6_native_joint_planner");
  ASSERT_EQ(provider.endpoint_bindings.size(), 1U);
  const auto &endpoint = provider.endpoint_bindings.front();
  EXPECT_EQ(endpoint.endpoint_role_id.value(),
            detail::kNativeExecutionRuntimeProjectionEndpointRoleId);
  EXPECT_EQ(endpoint.source_kind,
            core::CapabilityEndpointSourceKind::kRuntimeUnit);
  ASSERT_TRUE(std::holds_alternative<core::RuntimeUnitId>(endpoint.source_id));
  EXPECT_EQ(std::get<core::RuntimeUnitId>(endpoint.source_id).value(),
            test::kLite6NativeExecutionRuntimeId);
  EXPECT_EQ(endpoint.source_endpoint_role_id.value(),
            detail::kNativeExecutionRuntimeProjectionEndpointRoleId);
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
            test::kLite6NativeExecutionRuntimeId);
  ASSERT_EQ(group.products.size(), 1U);
  const auto &product = group.products.front();
  EXPECT_EQ(product.product_kind,
            core::RetainedProductKind::kRuntimeUnitConfiguration);
  EXPECT_EQ(product.product_id.value(),
            detail::native_execution_runtime_product_id(group.runtime_unit_id));
  EXPECT_EQ(product.product_contract_id.value(),
            detail::kNativeExecutionRuntimeProductContractId);
  EXPECT_EQ(product.product_contract_revision,
            detail::native_execution_runtime_product_contract_revision());
  ASSERT_EQ(product.artifact_refs.size(), 2U);
  const std::string manifest = require_product_artifact_bytes(
      product, detail::kNativeExecutionRuntimeManifestArtifactRole, raw_store);
  const std::string launch_descriptor = require_product_artifact_bytes(
      product, detail::kNativeExecutionRuntimeLaunchDescriptorArtifactRole,
      raw_store);
  EXPECT_EQ(launch_descriptor,
            detail::native_execution_runtime_launch_descriptor());
  const auto parsed_manifest =
      detail::parse_and_verify_native_execution_runtime_manifest(manifest);
  const auto& retained_liveness =
      native_execution_runtime_contract::manager_liveness_policy();
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
      R"({"collision_monitor_period_ms":50,"execution_tick_hz":100,"initial_joint_positions_rad":[0,0,1.57,0,0,0],"mode":"simulated","planner_rng_seed":12345})");
  EXPECT_NO_THROW(
      detail::require_native_execution_runtime_product_matches_retained_bundle(
          manifest, launch_descriptor, bundle->value(), group.runtime_unit_id));
  const auto runtime_configuration =
      native_execution_runtime_contract::require_configuration(
          *bundle, group.runtime_unit_id);
  EXPECT_EQ(runtime_configuration.planner_rng_seed, 12345U);
  EXPECT_EQ(runtime_configuration.initial_joint_positions_rad,
            (std::vector<double>{0.0, 0.0, 1.57, 0.0, 0.0, 0.0}));
  EXPECT_EQ(runtime_configuration.execution_tick_hz, 100U);
  EXPECT_DOUBLE_EQ(runtime_configuration.collision_monitor_period_ms, 50.0);
  const std::string changed_configuration = replace_once(
      manifest, "\"execution_tick_hz\":100", "\"execution_tick_hz\":99");
  EXPECT_NO_THROW(static_cast<void>(
      detail::parse_and_verify_native_execution_runtime_manifest(
          changed_configuration)));
  EXPECT_THROW(
      detail::require_native_execution_runtime_product_matches_retained_bundle(
          changed_configuration, launch_descriptor, bundle->value(),
          group.runtime_unit_id),
      RetainedBundleError);
  const std::string changed_liveness = replace_once(
      manifest, "\"heartbeat_period_ns\":250000000",
      "\"heartbeat_period_ns\":250000001");
  EXPECT_THROW(static_cast<void>(
                   detail::parse_and_verify_native_execution_runtime_manifest(
                       changed_liveness)),
               RetainedBundleError);

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
  EXPECT_EQ(bundle->value().catalogs.records.size(), 13U);
  EXPECT_EQ(catalog_store.size(), 13U);
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
  EXPECT_EQ(
      canonical_json(member(parsed_manifest, "configuration")),
      R"({"collision_monitor_period_ms":100,"execution_tick_hz":100,"initial_joint_positions_rad":[0,0,1.57,0,0,0],"isaac_profile":"lite6","mode":"commanded_sim","planner_rng_seed":12345,"runtime_collision_immediate_query_budget_ms":75,"runtime_collision_lookahead_cycle_budget_ms":350,"runtime_collision_lookahead_max_samples":32,"runtime_collision_lookahead_query_budget_ms":75,"runtime_collision_lookahead_s":2,"runtime_collision_lookahead_workers":4,"runtime_dynamic_body_max_age_cap_ms":1000,"validation_collision_workers":4})");
  EXPECT_NO_THROW(
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          manifest, launch_descriptor, bundle->value(), group.runtime_unit_id));

  const auto runtime_configuration =
      ros2_control_execution_runtime_contract::require_configuration(
          *bundle, group.runtime_unit_id);
  EXPECT_EQ(runtime_configuration.isaac_profile, "lite6");
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

  const std::string changed_configuration =
      replace_once(manifest, "\"isaac_profile\":\"lite6\"",
                   "\"isaac_profile\":\"ur10e\"");
  EXPECT_NO_THROW(static_cast<void>(
      detail::parse_and_verify_ros2_control_execution_runtime_manifest(
          changed_configuration)));
  EXPECT_THROW(
      detail::require_ros2_control_execution_runtime_product_matches_retained_bundle(
          changed_configuration, launch_descriptor, bundle->value(),
          group.runtime_unit_id),
      RetainedBundleError);
  EXPECT_NO_THROW(static_cast<void>(
      detail::RetainedBundleReloadAuthority::reload(bundle->canonical_bytes())));
}

TEST(RetainedBundleMaterializer,
     TwoNativeMotionLite6InstancesRetainDistinctRuntimeProductsAndEndpoints) {
  const auto fixture =
      test::make_two_lite6_same_type_native_motion_retention_fixture();
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
  ASSERT_EQ(bundle->value().cell_graph.runtime_units.size(), 2U);
  ASSERT_EQ(bundle->value().capability_provider_bindings.size(), 6U);
  ASSERT_EQ(bundle->value().compiled_products.runtime_unit_products.size(), 2U);
  ASSERT_EQ(bundle->value().runtime_contract_requirements.size(), 2U);
  const auto expected_requirements =
      detail::derive_runtime_contract_requirements(
          fixture.qualified.compiler_ir()
              .applicability()
              .composition()
              .graph(),
          fixture.qualified.deployment()
              .applicability_inputs()
              .value()
              .capability_provider_bindings,
          fixture.qualified.compiler_ir().applicability().capabilities(),
          fixture.qualified.compiler_ir()
              .applicability()
              .provider_endpoints(),
          fixture.qualified.compiler_ir().applicability().runtime_profiles(), {});
  EXPECT_EQ(bundle->value().runtime_contract_requirements,
            expected_requirements);

  const auto &graph =
      fixture.qualified.compiler_ir().applicability().composition().graph();
  ASSERT_EQ(graph.runtime_units.size(), 2U);
  const auto &first_runtime = graph.runtime_units.front().runtime_unit_id;
  const auto &second_runtime = graph.runtime_units.back().runtime_unit_id;
  const auto &logical_capabilities =
      fixture.qualified.compiler_ir().applicability().capabilities();
  const auto &logical_endpoints =
      fixture.qualified.compiler_ir().applicability().provider_endpoints();
  const LogicalCapabilityTemplate *second_runtime_capability = nullptr;
  for (const auto &capability : logical_capabilities) {
    const auto *owner =
        std::get_if<CapabilityProviderCapabilityOwner>(&capability.owner);
    if (owner == nullptr) {
      continue;
    }
    const auto endpoint = std::find_if(
        logical_endpoints.begin(), logical_endpoints.end(),
        [&](const auto &candidate) {
          const auto *source =
              std::get_if<RuntimeEndpointSourceTemplate>(&candidate.source);
          return candidate.capability_provider_binding_id ==
                     owner->capability_provider_binding_id &&
                 candidate.capability_provider_binding_revision ==
                     owner->capability_provider_binding_revision &&
                 source != nullptr && source->runtime_unit_id == second_runtime;
        });
    if (endpoint != logical_endpoints.end()) {
      second_runtime_capability = &capability;
      break;
    }
  }
  ASSERT_NE(second_runtime_capability, nullptr);
  const std::vector<LogicalCapabilityTemplate> isolated_capability{
      *second_runtime_capability};
  const auto isolated_requirements =
      detail::derive_runtime_contract_requirements(
          graph,
          fixture.qualified.deployment()
              .applicability_inputs()
              .value()
              .capability_provider_bindings,
          isolated_capability, logical_endpoints,
          fixture.qualified.compiler_ir().applicability().runtime_profiles(),
          {});
  const auto isolated_ref = core::RuntimeCapabilityContractRefDTO{
      second_runtime_capability->capability_id,
      second_runtime_capability->capability_definition_revision};
  for (const auto &row : isolated_requirements) {
    const bool has_isolated_capability = std::binary_search(
        row.requirement.required_capabilities.begin(),
        row.requirement.required_capabilities.end(), isolated_ref,
        [](const auto &left, const auto &right) {
          if (left.capability_id != right.capability_id) {
            return left.capability_id < right.capability_id;
          }
          return left.capability_definition_revision <
                 right.capability_definition_revision;
        });
    if (row.runtime_unit_id == first_runtime) {
      EXPECT_FALSE(has_isolated_capability);
    } else if (row.runtime_unit_id == second_runtime) {
      EXPECT_TRUE(has_isolated_capability);
    }
  }

  std::map<std::string, std::set<std::string>> capabilities_by_runtime;
  for (const auto &provider : bundle->value().capability_provider_bindings) {
    ASSERT_EQ(provider.endpoint_bindings.size(), 1U);
    const auto &endpoint = provider.endpoint_bindings.front();
    EXPECT_EQ(endpoint.endpoint_role_id.value(),
              detail::kNativeExecutionRuntimeProjectionEndpointRoleId);
    EXPECT_EQ(endpoint.source_kind,
              core::CapabilityEndpointSourceKind::kRuntimeUnit);
    ASSERT_TRUE(
        std::holds_alternative<core::RuntimeUnitId>(endpoint.source_id));
    EXPECT_EQ(endpoint.source_endpoint_role_id.value(),
              detail::kNativeExecutionRuntimeProjectionEndpointRoleId);
    capabilities_by_runtime[std::get<core::RuntimeUnitId>(endpoint.source_id)
                                .value()]
        .insert(provider.capability_id.value());
  }

  std::set<std::string> product_runtime_ids;
  std::set<std::string> product_ids;
  for (const auto &group :
       bundle->value().compiled_products.runtime_unit_products) {
    EXPECT_TRUE(
        product_runtime_ids.insert(group.runtime_unit_id.value()).second);
    ASSERT_EQ(group.products.size(), 1U);
    const auto &product = group.products.front();
    EXPECT_TRUE(product_ids.insert(product.product_id.value()).second);
    EXPECT_EQ(
        product.product_id.value(),
        detail::native_execution_runtime_product_id(group.runtime_unit_id));
    const std::string manifest = require_product_artifact_bytes(
        product, detail::kNativeExecutionRuntimeManifestArtifactRole,
        raw_store);
    const std::string launch_descriptor = require_product_artifact_bytes(
        product, detail::kNativeExecutionRuntimeLaunchDescriptorArtifactRole,
        raw_store);
    EXPECT_NO_THROW(
        detail::
            require_native_execution_runtime_product_matches_retained_bundle(
                manifest, launch_descriptor, bundle->value(),
                group.runtime_unit_id));
  }
  ASSERT_EQ(capabilities_by_runtime.size(), 2U);
  EXPECT_EQ(product_runtime_ids, ([&] {
              std::set<std::string> result;
              for (const auto &[runtime_id, capabilities] :
                   capabilities_by_runtime) {
                static_cast<void>(capabilities);
                result.insert(runtime_id);
              }
              return result;
            })());
  const std::set<std::string> expected_capabilities{
      "planning.motion.cartesian_path", "planning.motion.joint_goal",
      "planning.motion.pose_goal"};
  for (const auto &[runtime_id, capabilities] : capabilities_by_runtime) {
    SCOPED_TRACE(runtime_id);
    EXPECT_EQ(capabilities, expected_capabilities);
  }
}

TEST(RetainedBundleMaterializer,
     LogicalSceneRuntimeRequirementsMatchNativeAndRos2ControlOffers) {
  for (const bool ros2_control : {false, true}) {
    SCOPED_TRACE(ros2_control ? "ros2_control" : "native");
    const auto fixture =
        ros2_control
            ? test::make_lite6_ros2_control_motion_retention_fixture()
            : test::make_lite6_native_motion_retention_fixture();
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
  const auto fixture = test::make_lite6_native_motion_retention_fixture();
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
              native_execution_runtime_contract::kRuntimeRoleApiContractId)),
          core::ApiContractId(std::string(native_execution_runtime_contract::
                                              kProjectionEndpointApiContractId)),
          core::ApiContractId(std::string(
              native_execution_runtime_contract::kStatusEndpointApiContractId))}));

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
      test::make_two_lite6_same_type_native_motion_retention_fixture();
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
     NativeRuntimeProductRejectsMutableLaunchAndTransportSeams) {
  const auto fixture = test::make_lite6_native_motion_retention_fixture();
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
      product, detail::kNativeExecutionRuntimeManifestArtifactRole, raw_store);
  const std::string launch_descriptor = require_product_artifact_bytes(
      product, detail::kNativeExecutionRuntimeLaunchDescriptorArtifactRole,
      raw_store);

  struct Mutation final {
    std::string_view name;
    std::string from;
    std::string to;
  };
  const std::array<Mutation, 10U> mutations{{
      {"API version", "manyforge.native_execution_runtime_manifest.v1",
       "manyforge.native_execution_runtime_manifest.v2"},
      {"lifecycle", "manager_managed", "externally_managed"},
      {"process adapter", "manyforge.installed_native_target",
       "manyforge.arbitrary_command"},
      {"retrieval contract", "manyforge.retained_raw_artifact",
       "manyforge.mutable_local_artifact"},
      {"status endpoint", "manyforge.native_execution.status_direct",
       "manyforge.native_execution.status_discovered"},
      {"projection endpoint", "manyforge.planner_scene_projection.rpc",
       "manyforge.planner_scene_projection.ros_topic"},
      {"logical scene endpoint", "manyforge.logical_scene.services",
       "manyforge.logical_scene.discovered"},
      {"operating system", "\"operating_system\":\"linux\"",
       "\"operating_system\":\"other\""},
      {"launch environment", "manyforge.native_execution_host.abi.v1",
       "manyforge.caller_selected_environment"},
      {"runtime role", "\"runtime_role_id\":\"native_execution\"",
       "\"runtime_role_id\":\"foreign_role\""},
  }};
  for (const auto &mutation : mutations) {
    SCOPED_TRACE(mutation.name);
    EXPECT_THROW(static_cast<void>(
                     detail::parse_and_verify_native_execution_runtime_manifest(
                         replace_once(manifest, mutation.from, mutation.to))),
                 RetainedBundleError);
  }

  const auto parsed_manifest =
      detail::parse_and_verify_native_execution_runtime_manifest(manifest);
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
                   detail::parse_and_verify_native_execution_runtime_manifest(
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
                   detail::parse_and_verify_native_execution_runtime_manifest(
                       canonical_json(CanonicalValue(std::move(injected))))),
               RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          detail::parse_and_verify_native_execution_runtime_manifest(
              std::string(detail::kMaxNativeExecutionRuntimeManifestBytes + 1U,
                          'x'))),
      RetainedBundleError);
  EXPECT_THROW(
      detail::require_native_execution_runtime_product_matches_retained_bundle(
          manifest,
          replace_once(launch_descriptor,
                       detail::kNativeExecutionRuntimeInstalledTargetId,
                       "caller_selected_target"),
          bundle->value(), group.runtime_unit_id),
      RetainedBundleError);
  EXPECT_THROW(
      detail::require_native_execution_runtime_product_matches_retained_bundle(
          manifest, launch_descriptor, bundle->value(),
          core::RuntimeUnitId("different_runtime")),
      RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     TwoSameTypeComponentsRetainDistinctTargetsResourcesAndDomains) {
  const auto fixture =
      test::make_two_lite6_same_type_controllable_retention_fixture();
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
  ASSERT_EQ(bundle->value().cell_graph.controllable_components.size(), 2U);
  ASSERT_EQ(bundle->value().cell_graph.control_domains.size(), 2U);
  ASSERT_EQ(bundle->value().cell_graph.control_bindings.size(), 2U);
  EXPECT_EQ(bundle->value().catalogs.records.size(), 8U);
  EXPECT_EQ(catalog_store.size(), 8U);
  EXPECT_EQ(bundle->value().compiled_products.control_domain_products.size(),
            2U);
  EXPECT_EQ(bundle->value().compiled_products.runtime_unit_products.size(), 2U);

  const auto &components = bundle->value().cell_graph.controllable_components;
  EXPECT_NE(components[0].controllable_component_id,
            components[1].controllable_component_id);
  EXPECT_EQ(components[0].component_type_id, components[1].component_type_id);
  EXPECT_EQ(components[0].component_type_revision,
            components[1].component_type_revision);

  std::set<std::string> domain_ids;
  for (const auto &group :
       bundle->value().compiled_products.control_domain_products) {
    EXPECT_TRUE(domain_ids.insert(group.control_domain_id.value()).second);
    ASSERT_EQ(group.products.size(), 1U);
    const std::string control_bytes = require_product_artifact_bytes(
        group.products.front(), detail::kControlDomainControlArtifactRole,
        raw_store);
    EXPECT_NO_THROW(detail::require_control_domain_control_matches(
        control_bytes, fixture.qualified.compiler_ir(),
        group.control_domain_id));
    const CanonicalValue control =
        detail::parse_and_verify_control_domain_control(control_bytes);
    ASSERT_EQ(array_member(control, "bindings").size(), 1U);
  }

  ASSERT_EQ(bundle->value().compiled_products.symbol_role_maps.size(), 1U);
  const std::string symbol_map = require_product_artifact_bytes(
      bundle->value().compiled_products.symbol_role_maps.front(),
      detail::kComposedModelSymbolRoleMapArtifactRole, raw_store);
  const CanonicalValue parsed =
      detail::parse_and_verify_composed_model_symbol_role_map(symbol_map);
  const auto &binding_maps =
      array_member(parsed, "control_binding_coordinate_maps");
  const auto &targets = array_member(parsed, "target_role_maps");
  ASSERT_EQ(binding_maps.size(), 2U);
  ASSERT_EQ(targets.size(), 2U);

  std::set<std::string> binding_ids;
  std::set<std::string> component_ids;
  std::set<std::string> resource_ids;
  for (const auto &map : binding_maps) {
    EXPECT_TRUE(
        binding_ids.insert(string_member(map, "control_binding_id")).second);
    EXPECT_TRUE(
        component_ids.insert(string_member(map, "controllable_component_id"))
            .second);
    EXPECT_EQ(string_member(map, "command_target_id"), test::kLite6ArmTargetId);
    for (const auto &resource : array_member(map, "resources")) {
      EXPECT_TRUE(
          resource_ids.insert(string_member(resource, "resource_id")).second);
    }
  }
  EXPECT_EQ(resource_ids.size(), 12U);
}

TEST(RetainedBundleMaterializer,
     AssistantSettingsRetainReloadAndRequalifyExactly) {
  const auto fixture = test::make_lite6_assistant_retention_fixture();
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

  CanonicalValue::Array resources =
      array_member(bundle->normalized_root(), "external_resources");
  ASSERT_EQ(resources.size(), 2U);
  ASSERT_EQ(string_member(resources.front(), "external_resource_alias"),
            "assistant_bridge");
  resources.front() = replace_object_member(resources.front(), "resource_kind",
                                            CanonicalValue("model_endpoint"));
  CanonicalValue changed_resources_root =
      replace_root_member(bundle->normalized_root(), "external_resources",
                          CanonicalValue(std::move(resources)));
  changed_resources_root =
      reidentify_retained_bundle(std::move(changed_resources_root));
  EXPECT_THROW(static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
                   canonical_json(changed_resources_root))),
               RetainedBundleError);
}

TEST(RetainedBundleMaterializer,
     AggregateNativeAssistantProductRetainsOneRuntimeAndDraftProjection) {
  const auto fixture =
      test::make_lite6_native_motion_arm_gripper_aggregate_assistant_retention_fixture();
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
  EXPECT_EQ(bundle->value().cell_graph.cell_id.value(),
            "i6_lite6_arm_gripper_aggregate_cell");
  EXPECT_EQ(bundle->value().cell_graph.runtime_units.size(), 1U);
  EXPECT_EQ(bundle->value().compiled_products.runtime_unit_products.size(),
            1U);
  ASSERT_TRUE(bundle->value().assistant.has_value());
  EXPECT_TRUE(fixture.qualified.compiler_ir()
                  .applicability()
                  .assistant_settings()
                  .enabled());
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
            "lite6_binding_state_stop");
  EXPECT_DOUBLE_EQ(std::get<double>(member(member(stop, "deadline_budget"),
                                           "outer_duration_ns")
                                        .storage()),
                   1'000'000'000.0);

  const auto &stages = array_member(stop, "escalation_stages");
  ASSERT_EQ(stages.size(), 2U);
  const CanonicalValue &nominal = stages.front();
  EXPECT_EQ(string_member(nominal, "stop_escalation_stage_id"), "nominal");
  EXPECT_EQ(string_member(nominal, "stage_kind"), "nominal");
  const CanonicalValue &command = member(nominal, "containment_command");
  EXPECT_EQ(string_member(command, "command_phase_id"), "nominal_command");
  EXPECT_EQ(string_member(command, "command_kind"), "controller_goal_cancel");
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      member(command, "provider_contract_ref").storage()));
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      member(command, "registered_command_kind_id").storage()));
  const auto &evidence = array_member(nominal, "all_of_evidence_requirements");
  ASSERT_EQ(evidence.size(), 1U);
  EXPECT_EQ(string_member(evidence.front(), "evidence_kind"), "motion_rest");
  EXPECT_FALSE(std::holds_alternative<std::nullptr_t>(
      member(evidence.front(), "provider_contract_ref").storage()));
  EXPECT_EQ(string_member(member(evidence.front(), "source_requirement"),
                          "source_kind"),
            "binding_state_authority");
  EXPECT_EQ(string_member(member(evidence.front(), "source_requirement"),
                          "endpoint_role_id"),
            "state");
  EXPECT_TRUE(array_member(nominal, "provider_evidence_endpoints").empty());
  const auto &binding_sources =
      array_member(nominal, "binding_state_authority_sources");
  ASSERT_EQ(binding_sources.size(), 1U);
  EXPECT_EQ(string_member(binding_sources.front(), "evidence_item_id"),
            "nominal_rest");
  EXPECT_EQ(string_member(binding_sources.front(), "state_authority_class"),
            "measured");
  EXPECT_EQ(string_member(binding_sources.front(), "state_endpoint_role_id"),
            "state");
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
  EXPECT_EQ(string_member(hard_command, "command_phase_id"), "hard_command");
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
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      fixture);
  test::InMemoryCatalogArtifactStoreFixture catalog_store;
  test::InMemoryRawArtifactStoreFixture raw_store;

  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), catalog_store, raw_store);

  ASSERT_NE(bundle, nullptr);
  ASSERT_EQ(bundle->value().compiled_products.control_domain_products.size(),
            1U);
  const auto &product = bundle->value()
                            .compiled_products.control_domain_products.front()
                            .products.front();
  const std::string bytes = require_product_artifact_bytes(
      product, detail::kControlDomainControlArtifactRole, raw_store);
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
      fixture.qualified,
      detail::RetentionMaterializerTestAccess::make_context(), input_source,
      fixture.structural_preparations(), second_catalog_store,
      second_raw_store);
  ASSERT_NE(regenerated, nullptr);
  EXPECT_EQ(regenerated->retained_bundle_id(), bundle->retained_bundle_id());
  EXPECT_EQ(regenerated->canonical_bytes(), bundle->canonical_bytes());
  EXPECT_EQ(require_product_artifact_bytes(
                regenerated->value()
                    .compiled_products.control_domain_products.front()
                    .products.front(),
                detail::kControlDomainControlArtifactRole, second_raw_store),
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
  const auto first = materialize_empty("i3_empty_cell", first_store);
  const auto second = materialize_empty("i3_empty_cell", second_store);

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
  const auto first = materialize_empty("i3_bound_cell_a", first_store);
  const auto second = materialize_empty("i3_bound_cell_b", second_store);

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
  const auto first = materialize_empty("i3_empty_cell_a", first_store);
  const auto second = materialize_empty("i3_empty_cell_b", second_store);

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

} // namespace
} // namespace manyforge::deployment
