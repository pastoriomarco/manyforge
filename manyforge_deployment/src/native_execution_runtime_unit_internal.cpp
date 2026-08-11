#include "native_execution_runtime_unit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "stop_qualification_evidence_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kRuntimeProfileConfigurationSchemaId =
    kNativeExecutionRuntimeConfigurationSchemaId;
constexpr std::string_view kRuntimeProfileConfigurationSchemaRevision =
    kNativeExecutionRuntimeConfigurationSchemaRevision;

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view field) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail_retained_bundle(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue::Object &object, std::string_view name,
               std::string_view field) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [&](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    fail_retained_bundle(std::string(field) + " is missing " +
                         std::string(name));
  }
  return found->second;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view field) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail_retained_bundle(std::string(field) + " must be a string");
  }
  return *text;
}

[[nodiscard]] CanonicalValue
json_value_to_canonical(const core::JsonValueDTO &value);

[[nodiscard]] CanonicalValue
json_object_to_canonical(const core::JsonObjectDTO &object) {
  CanonicalValue::Object result;
  result.reserve(object.size());
  for (const auto &[name, value] : object) {
    result.emplace_back(name, json_value_to_canonical(value));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue
json_value_to_canonical(const core::JsonValueDTO &value) {
  return std::visit(
      [](const auto &current) -> CanonicalValue {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<Value, bool> ||
                             std::is_same_v<Value, double> ||
                             std::is_same_v<Value, std::string>) {
          return CanonicalValue(current);
        } else if constexpr (std::is_same_v<Value, core::JsonValueDTO::Array>) {
          CanonicalValue::Array array;
          array.reserve(current.size());
          for (const auto &item : current) {
            array.push_back(json_value_to_canonical(item));
          }
          return CanonicalValue(std::move(array));
        } else {
          return json_object_to_canonical(current);
        }
      },
      value.value);
}

[[nodiscard]] core::JsonValueDTO
canonical_to_json_value(const CanonicalValue &value);

[[nodiscard]] core::JsonObjectDTO
canonical_to_json_object(const CanonicalValue &value, std::string_view field) {
  const auto &object = require_object(value, field);
  core::JsonObjectDTO result;
  for (const auto &[name, member] : object) {
    result.emplace(name, canonical_to_json_value(member));
  }
  return result;
}

[[nodiscard]] core::JsonValueDTO
canonical_to_json_value(const CanonicalValue &value) {
  return std::visit(
      [&value](const auto &current) -> core::JsonValueDTO {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t> ||
                      std::is_same_v<Value, bool> ||
                      std::is_same_v<Value, double> ||
                      std::is_same_v<Value, std::string>) {
          return core::JsonValueDTO{current};
        } else if constexpr (std::is_same_v<Value, CanonicalValue::Array>) {
          core::JsonValueDTO::Array array;
          array.reserve(current.size());
          for (const auto &item : current) {
            array.push_back(canonical_to_json_value(item));
          }
          return core::JsonValueDTO{std::move(array)};
        } else {
          return core::JsonValueDTO{
              canonical_to_json_object(value, "runtime_configuration")};
        }
      },
      value.storage());
}

[[nodiscard]] CanonicalValue require_qualified_runtime_configuration(
    const core::JsonObjectDTO &configuration) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      core::SchemaId(std::string(kRuntimeProfileConfigurationSchemaId)),
      core::SchemaRevision(
          std::string(kRuntimeProfileConfigurationSchemaRevision)));
  if (schema == nullptr) {
    fail_retained_bundle(
        "native execution configuration schema is absent from this build");
  }
  const CanonicalValue candidate = json_object_to_canonical(configuration);
  try {
    const auto normalized =
        validate_and_normalize_build_qualified_value(*schema, candidate);
    if (normalized.normalized_value() != candidate) {
      fail_retained_bundle(
          "native execution configuration is not in canonical normalized form");
    }
    return normalized.normalized_value();
  } catch (const ClosedSchemaError &error) {
    fail_retained_bundle(std::string("native execution configuration: ") +
                         error.what());
  } catch (const CanonicalValueError &error) {
    fail_retained_bundle(std::string("native execution configuration: ") +
                         error.what());
  }
}

[[nodiscard]] std::string contract_revision(std::string_view domain,
                                            std::string_view description) {
  std::string preimage("manyforge.native-execution-runtime-contract.v1");
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(description);
  return sha256_digest(preimage);
}

} // namespace

CanonicalValue
native_execution_runtime_manifest_projection(const core::RuntimeUnitDTO &unit) {
  const CanonicalValue configuration =
      require_qualified_runtime_configuration(unit.configuration);
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kNativeExecutionRuntimeManifestApiVersion))},
      {"runtime_unit_id", CanonicalValue(unit.runtime_unit_id.value())},
      {"runtime_product_id", CanonicalValue(native_execution_runtime_product_id(
                                 unit.runtime_unit_id))},
      {"configuration", configuration},
      {"runtime_profile",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_profile_id",
            CanonicalValue(unit.runtime_profile_id.value())},
           {"runtime_profile_revision",
            CanonicalValue(unit.runtime_profile_revision.value())},
           {"implementation_id", CanonicalValue(std::string(
                                     kNativeExecutionRuntimeImplementationId))},
           {"implementation_revision",
            CanonicalValue(native_execution_runtime_implementation_revision())},
       })},
      {"process_adapter",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
           {"process_adapter_contract_id",
            CanonicalValue(
                std::string(kNativeExecutionRuntimeProcessAdapterContractId))},
           {"process_adapter_contract_revision",
            CanonicalValue(
                native_execution_runtime_process_adapter_revision())},
           {"retrieval_contract_id",
            CanonicalValue(
                std::string(kNativeExecutionRuntimeRetrievalContractId))},
           {"retrieval_contract_revision",
            CanonicalValue(
                native_execution_runtime_retrieval_contract_revision())},
       })},
      {"runtime_role_mappings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"runtime_role_id",
                CanonicalValue(std::string(kNativeExecutionRuntimeRoleId))},
               {"implementation_role_id",
                CanonicalValue(
                    std::string(kNativeExecutionRuntimeImplementationId))},
           }),
       })},
      {"source_endpoint_mappings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(std::string(
                    kNativeExecutionRuntimeLogicalSceneEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kNativeExecutionRuntimeLogicalSceneEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    native_execution_runtime_logical_scene_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(
                    std::string(kNativeExecutionRuntimeStatusEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kNativeExecutionRuntimeStatusEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    native_execution_runtime_status_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(std::string(
                    kNativeExecutionRuntimeProjectionEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kNativeExecutionRuntimeProjectionEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    native_execution_runtime_projection_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
       })},
      {"launchable_artifact",
       CanonicalValue(CanonicalValue::Object{
           {"artifact_role_id",
            CanonicalValue(std::string(
                kNativeExecutionRuntimeLaunchDescriptorArtifactRole))},
           {"media_type",
            CanonicalValue(
                std::string(kNativeExecutionRuntimeLaunchDescriptorMediaType))},
           {"content_digest",
            CanonicalValue(
                sha256_digest(native_execution_runtime_launch_descriptor()))},
       })},
      {"platform_constraints",
       CanonicalValue(CanonicalValue::Object{
           {"operating_system", CanonicalValue(std::string(
                                    kNativeExecutionRuntimeOperatingSystem))},
           {"launch_environment_contract",
            CanonicalValue(
                std::string(kNativeExecutionRuntimeLaunchEnvironmentContract))},
       })},
  });
}

namespace {

[[nodiscard]] const core::RuntimeUnitDTO &
require_retained_runtime_unit(const core::RetainedBundleDTO &bundle,
                              const core::RuntimeUnitId &runtime_unit_id) {
  const auto found = std::find_if(
      bundle.cell_graph.runtime_units.begin(),
      bundle.cell_graph.runtime_units.end(), [&](const auto &unit) {
        return unit.runtime_unit_id == runtime_unit_id;
      });
  if (found == bundle.cell_graph.runtime_units.end()) {
    fail_retained_bundle(
        "native execution product has no exact retained graph subject");
  }
  return *found;
}

[[nodiscard]] const core::RetainedRuntimeUnitProductsDTO &
require_runtime_product_group(const core::RetainedBundleDTO &bundle,
                              const core::RuntimeUnitId &runtime_unit_id) {
  const auto found =
      std::find_if(bundle.compiled_products.runtime_unit_products.begin(),
                   bundle.compiled_products.runtime_unit_products.end(),
                   [&](const auto &group) {
                     return group.runtime_unit_id == runtime_unit_id;
                   });
  if (found == bundle.compiled_products.runtime_unit_products.end() ||
      found->products.size() != 1U) {
    fail_retained_bundle(
        "native execution product has no exact retained product group");
  }
  return *found;
}

[[nodiscard]] const core::RetainedArtifactRefDTO &
require_artifact(const core::RetainedProductDTO &product,
                 std::string_view role) {
  const core::RetainedArtifactRefDTO *match = nullptr;
  for (const auto &artifact : product.artifact_refs) {
    if (artifact.artifact_role_id.value() != role) {
      continue;
    }
    if (match != nullptr) {
      fail_retained_bundle("native execution product repeats an artifact role");
    }
    match = &artifact;
  }
  if (match == nullptr) {
    fail_retained_bundle("native execution product omits an artifact role");
  }
  return *match;
}

[[nodiscard]] const core::RetainedArtifactRefDTO *find_artifact(
    const core::RetainedProductDTO &product, std::string_view role) {
  const core::RetainedArtifactRefDTO *match = nullptr;
  for (const auto &artifact : product.artifact_refs) {
    if (artifact.artifact_role_id.value() != role) {
      continue;
    }
    if (match != nullptr) {
      fail_retained_bundle("native execution product repeats an artifact role");
    }
    match = &artifact;
  }
  return match;
}

} // namespace

std::string native_execution_runtime_profile_document() {
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.runtime_profile.v1")},
      {"runtime_profile_id",
       CanonicalValue(std::string(kNativeExecutionRuntimeProfileId))},
      {"configuration_schema_ref",
       CanonicalValue(CanonicalValue::Object{
           {"schema_id",
            CanonicalValue(std::string(kRuntimeProfileConfigurationSchemaId))},
           {"schema_revision",
            CanonicalValue(
                std::string(kRuntimeProfileConfigurationSchemaRevision))},
       })},
      {"process_contract",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
       })},
      {"supervision_contract",
       CanonicalValue(CanonicalValue::Object{
           {"status_endpoint_role_id",
            CanonicalValue(
                std::string(kNativeExecutionRuntimeStatusEndpointRoleId))},
       })},
      {"offered_runtime_role_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(std::string(kNativeExecutionRuntimeRoleId))})},
      {"source_endpoint_role_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(
               std::string(kNativeExecutionRuntimeLogicalSceneEndpointRoleId)),
           CanonicalValue(
               std::string(kNativeExecutionRuntimeStatusEndpointRoleId)),
           CanonicalValue(
               std::string(kNativeExecutionRuntimeProjectionEndpointRoleId)),
       })},
  });
  std::string preimage("manyforge.content-identity.v1");
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/runtime_profile");
  preimage.push_back('\0');
  preimage.append("manyforge.runtime_profile.v1");
  preimage.push_back('\0');
  preimage.append(canonical_json(identity));
  auto root = identity.as_object();
  root.emplace_back("runtime_profile_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

std::string native_execution_runtime_profile_revision() {
  const auto root =
      parse_strict_json_model_yaml(native_execution_runtime_profile_document())
          .root();
  return require_string(
      require_member(require_object(root, "native_execution_runtime_profile"),
                     "runtime_profile_revision",
                     "native_execution_runtime_profile"),
      "native_execution_runtime_profile.runtime_profile_revision");
}

bool native_execution_runtime_configuration_is_qualified(
    const core::JsonObjectDTO &configuration) noexcept {
  try {
    static_cast<void>(require_qualified_runtime_configuration(configuration));
    return true;
  } catch (...) {
    return false;
  }
}

std::string native_execution_runtime_implementation_revision() {
  return contract_revision(
      "runtime-implementation",
      "native-execution-host;deployment-bound-startup;operation-bound-program;"
      "projection-handoff-v1;private-rpc-v1;retained-runtime-config-v2;"
      "logical-scene-services-v1;manager-managed-product-composition;"
      "build-owned-candidate-worker;sha256-projection-content;"
      "atomic-resource-table-admission;"
      "resource-derived-active-configurations");
}

std::string native_execution_runtime_process_adapter_revision() {
  return contract_revision(
      "process-adapter",
      "manager-managed;installed-target;fixed-argv;direct-child-epoch;"
      "inherited-seqpacket-channel;same-user-direct-parent;"
      "command-fenced-readiness;fixed-sibling-candidate-worker");
}

std::string native_execution_runtime_retrieval_contract_revision() {
  return contract_revision(
      "retrieval-contract",
      "retained-raw-artifact;digest-and-size-verified-launch-descriptor");
}

std::string native_execution_runtime_status_endpoint_revision() {
  return contract_revision(
      "status-endpoint",
      "adapter-private;native-execution-status-v1;direct-child-epoch;"
      "source-sequence;phase;health;product;configuration;roles;fence");
}

std::string native_execution_runtime_projection_endpoint_revision() {
  return contract_revision(
      "projection-endpoint",
      "planner-scene-projection-rpc-v1;exact-native-binding;bounded-framing;"
      "request-replay;health;stage;activate;retire;discard;rebuild;lease;"
      "sha256-content-identity");
}

std::string native_execution_runtime_logical_scene_endpoint_revision() {
  return contract_revision(
      "logical-scene-endpoint",
      "adapter-private;logical-scene-services-v1;reference;query;shape;"
      "compare-and-commit-edit;collision;trace;exact-revision-and-lease");
}

std::string native_execution_runtime_launch_descriptor() {
  return canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.installed_target_launch.v1")},
      {"implementation_id",
       CanonicalValue(std::string(kNativeExecutionRuntimeImplementationId))},
      {"implementation_revision",
       CanonicalValue(native_execution_runtime_implementation_revision())},
      {"target_id",
       CanonicalValue(std::string(kNativeExecutionRuntimeInstalledTargetId))},
  }));
}

std::string native_execution_runtime_product_id(
    const core::RuntimeUnitId &runtime_unit_id) {
  require_retained_portable_identity(runtime_unit_id.value(),
                                     "runtime_unit_id");
  std::string preimage("manyforge.runtime-unit-product-id.v1");
  preimage.push_back('\0');
  preimage.append(runtime_unit_id.value());
  return "runtime_unit_configuration_" + sha256_digest(preimage).substr(7U);
}

CanonicalValue parse_and_verify_native_execution_runtime_manifest(
    std::string_view canonical_bytes) {
  if (canonical_bytes.empty() ||
      canonical_bytes.size() > kMaxNativeExecutionRuntimeManifestBytes) {
    fail_retained_bundle(
        "native execution runtime manifest is empty or exceeds its bound");
  }
  CanonicalValue root;
  try {
    root = parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const std::exception &error) {
    fail_retained_bundle(
        std::string("native execution runtime manifest is invalid: ") +
        error.what());
  }
  if (canonical_json(root) != canonical_bytes) {
    fail_retained_bundle(
        "native execution runtime manifest is not canonical JSON");
  }
  const auto &object =
      require_object(root, "native_execution_runtime_manifest");
  const auto &api =
      require_string(require_member(object, "api_version",
                                    "native_execution_runtime_manifest"),
                     "native_execution_runtime_manifest.api_version");
  const auto &runtime_unit_id =
      require_string(require_member(object, "runtime_unit_id",
                                    "native_execution_runtime_manifest"),
                     "native_execution_runtime_manifest.runtime_unit_id");
  require_retained_portable_identity(runtime_unit_id, "runtime_unit_id");
  const auto configuration = canonical_to_json_object(
      require_member(object, "configuration",
                     "native_execution_runtime_manifest"),
      "native_execution_runtime_manifest.configuration");
  if (!native_execution_runtime_configuration_is_qualified(configuration)) {
    fail_retained_bundle(
        "native execution runtime manifest has an unqualified configuration");
  }
  const core::RuntimeUnitDTO expected{
      core::RuntimeUnitId(runtime_unit_id),
      core::RuntimeProfileId(std::string(kNativeExecutionRuntimeProfileId)),
      core::RuntimeProfileRevision(native_execution_runtime_profile_revision()),
      {},
      configuration,
  };
  if (api != kNativeExecutionRuntimeManifestApiVersion ||
      root != native_execution_runtime_manifest_projection(expected)) {
    fail_retained_bundle(
        "native execution runtime manifest differs from its closed contract");
  }
  return root;
}

void require_native_execution_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes, std::string_view launch_descriptor_bytes,
    std::optional<std::string_view> implementation_build_identity_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id) {
  const auto manifest =
      parse_and_verify_native_execution_runtime_manifest(manifest_bytes);
  if (launch_descriptor_bytes != native_execution_runtime_launch_descriptor() ||
      launch_descriptor_bytes.size() >
          kMaxNativeExecutionLaunchDescriptorBytes) {
    fail_retained_bundle(
        "native execution launch descriptor differs from the build-owned "
        "artifact");
  }
  const auto &unit =
      require_retained_runtime_unit(retained_bundle, runtime_unit_id);
  if (unit.runtime_profile_id.value() != kNativeExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          native_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty() ||
      !native_execution_runtime_configuration_is_qualified(
          unit.configuration) ||
      manifest != native_execution_runtime_manifest_projection(unit)) {
    fail_retained_bundle(
        "native execution manifest differs from its retained graph/profile");
  }
  const auto &group =
      require_runtime_product_group(retained_bundle, runtime_unit_id);
  const auto &product = group.products.front();
  if (product.product_id.value() !=
          native_execution_runtime_product_id(runtime_unit_id) ||
      product.product_kind !=
          core::RetainedProductKind::kRuntimeUnitConfiguration ||
      product.product_contract_id.value() !=
          kNativeExecutionRuntimeProductContractId ||
      product.product_contract_revision !=
          native_execution_runtime_product_contract_revision() ||
      product.artifact_refs.size() !=
          2U + static_cast<std::size_t>(
                   implementation_build_identity_bytes.has_value())) {
    fail_retained_bundle(
        "native execution retained product has an incoherent identity");
  }
  const auto &manifest_ref =
      require_artifact(product, kNativeExecutionRuntimeManifestArtifactRole);
  const auto &launch_ref = require_artifact(
      product, kNativeExecutionRuntimeLaunchDescriptorArtifactRole);
  const auto *build_identity_ref = find_artifact(
      product, kNativeExecutionRuntimeBuildIdentityArtifactRole);
  if (manifest_ref.media_type != kNativeExecutionRuntimeManifestMediaType ||
      manifest_ref.content_digest.value() != sha256_digest(manifest_bytes) ||
      manifest_ref.byte_size != manifest_bytes.size() ||
      launch_ref.media_type !=
          kNativeExecutionRuntimeLaunchDescriptorMediaType ||
      launch_ref.content_digest.value() !=
          sha256_digest(launch_descriptor_bytes) ||
      launch_ref.byte_size != launch_descriptor_bytes.size() ||
      (build_identity_ref != nullptr) !=
          implementation_build_identity_bytes.has_value()) {
    fail_retained_bundle(
        "native execution product artifact table differs from its bytes");
  }
  if (implementation_build_identity_bytes.has_value()) {
    const auto identity = parse_stop_implementation_build_identity(
        *implementation_build_identity_bytes);
    if (implementation_build_identity_bytes->empty() ||
        build_identity_ref->media_type !=
            kNativeExecutionRuntimeBuildIdentityMediaType ||
        build_identity_ref->content_digest.value() !=
            sha256_digest(*implementation_build_identity_bytes) ||
        build_identity_ref->byte_size !=
            implementation_build_identity_bytes->size() ||
        identity.implementation_id !=
            kNativeExecutionRuntimeImplementationId ||
        identity.installed_target_id !=
            kNativeExecutionRuntimeInstalledTargetId) {
      fail_retained_bundle(
          "native execution build identity differs from its exact runtime "
          "product");
    }
  }

  bool source_found = false;
  for (const auto &source : retained_bundle.provenance.source_records) {
    std::visit(
        [&](const auto &typed) {
          using Source = std::remove_cvref_t<decltype(typed)>;
          if constexpr (Source::kRecordKind ==
                        core::CatalogRecordKind::kRuntimeProfile) {
            if (typed.record_id == unit.runtime_profile_id &&
                typed.revision == unit.runtime_profile_revision) {
              if (source_found) {
                fail_retained_bundle(
                    "native execution runtime profile source is duplicated");
              }
              source_found = true;
            }
          }
        },
        source);
  }
  const auto output = std::find_if(
      retained_bundle.provenance.output_records.begin(),
      retained_bundle.provenance.output_records.end(),
      [&](const auto &row) { return row.product_id == product.product_id; });
  if (!source_found ||
      output == retained_bundle.provenance.output_records.end() ||
      output->product_kind != product.product_kind ||
      output->content_digest != retained_product_output_digest(product)) {
    fail_retained_bundle(
        "native execution product lacks exact source/output provenance");
  }
}

} // namespace manyforge::deployment::detail

namespace manyforge::deployment::native_execution_runtime_contract {

Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id) {
  if (!verify_retained_bundle_identity(retained_bundle)) {
    throw RetainedBundleError(
        "native runtime configuration received an unverifiable bundle");
  }
  const auto &unit = detail::require_retained_runtime_unit(
      retained_bundle.value(), runtime_unit_id);
  if (unit.runtime_profile_id.value() !=
          detail::kNativeExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          detail::native_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty() ||
      !detail::native_execution_runtime_configuration_is_qualified(
          unit.configuration)) {
    throw RetainedBundleError(
        "native runtime unit differs from the build-qualified profile");
  }

  const auto require_member =
      [&](const std::string_view name) -> const core::JsonValueDTO & {
    const auto found = unit.configuration.find(name);
    if (found == unit.configuration.end()) {
      throw RetainedBundleError("native runtime configuration is missing " +
                                std::string(name));
    }
    return found->second;
  };
  const auto require_number = [&](const std::string_view name) {
    const auto *number = std::get_if<double>(&require_member(name).value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError("native runtime configuration has invalid " +
                                std::string(name));
    }
    return *number;
  };
  const auto require_uint32 = [&](const std::string_view name) {
    const double number = require_number(name);
    if (std::trunc(number) != number || number < 0.0 ||
        number >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      throw RetainedBundleError("native runtime configuration has invalid " +
                                std::string(name));
    }
    return static_cast<std::uint32_t>(number);
  };

  const auto *mode = std::get_if<std::string>(&require_member("mode").value);
  const auto *positions = std::get_if<core::JsonValueDTO::Array>(
      &require_member("initial_joint_positions_rad").value);
  if (mode == nullptr || *mode != "simulated" || positions == nullptr ||
      positions->empty()) {
    throw RetainedBundleError(
        "native runtime configuration has an unsupported mode or initial "
        "state");
  }

  Configuration result;
  result.planner_rng_seed = require_uint32("planner_rng_seed");
  result.execution_tick_hz = require_uint32("execution_tick_hz");
  result.collision_monitor_period_ms =
      require_number("collision_monitor_period_ms");
  result.initial_joint_positions_rad.reserve(positions->size());
  for (const auto &position : *positions) {
    const auto *number = std::get_if<double>(&position.value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError(
          "native runtime configuration has an invalid initial joint state");
    }
    result.initial_joint_positions_rad.push_back(*number);
  }
  if (result.execution_tick_hz == 0U ||
      !(result.collision_monitor_period_ms > 0.0)) {
    throw RetainedBundleError(
        "native runtime configuration has an invalid cadence");
  }
  return result;
}

std::string implementation_revision() {
  return detail::native_execution_runtime_implementation_revision();
}

} // namespace manyforge::deployment::native_execution_runtime_contract
