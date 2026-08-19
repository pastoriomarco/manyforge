#include "ros2_control_execution_runtime_unit_internal.hpp"

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
#include "manyforge_deployment/runtime_contract_registry.hpp"
#include "stop_qualification_evidence_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

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
        } else if constexpr (std::is_same_v<Value,
                                            core::JsonValueDTO::Array>) {
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
        } else if constexpr (std::is_same_v<Value,
                                            CanonicalValue::Array>) {
          core::JsonValueDTO::Array array;
          array.reserve(current.size());
          for (const auto &item : current) {
            array.push_back(canonical_to_json_value(item));
          }
          return core::JsonValueDTO{std::move(array)};
        } else {
          return core::JsonValueDTO{canonical_to_json_object(
              value, "ros2_control_runtime_configuration")};
        }
      },
      value.storage());
}

[[nodiscard]] CanonicalValue require_qualified_runtime_configuration(
    const core::JsonObjectDTO &configuration) {
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      core::SchemaId(
          std::string(kRos2ControlExecutionRuntimeConfigurationSchemaId)),
      core::SchemaRevision(std::string(
          kRos2ControlExecutionRuntimeConfigurationSchemaRevision)));
  if (schema == nullptr) {
    fail_retained_bundle(
        "ros2_control execution configuration schema is absent from this "
        "build");
  }
  const CanonicalValue candidate = json_object_to_canonical(configuration);
  try {
    const auto normalized =
        validate_and_normalize_build_qualified_value(*schema, candidate);
    if (normalized.normalized_value() != candidate) {
      fail_retained_bundle("ros2_control execution configuration is not in "
                           "canonical normalized form");
    }
    return normalized.normalized_value();
  } catch (const ClosedSchemaError &error) {
    fail_retained_bundle(std::string("ros2_control execution configuration: ") +
                         error.what());
  } catch (const CanonicalValueError &error) {
    fail_retained_bundle(std::string("ros2_control execution configuration: ") +
                         error.what());
  }
}

[[nodiscard]] std::string contract_revision(std::string_view domain,
                                            std::string_view description) {
  std::string preimage(
      "manyforge.ros2-control-execution-runtime-contract.v1");
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(description);
  return sha256_digest(preimage);
}

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
        "ros2_control execution product has no exact retained graph subject");
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
        "ros2_control execution product has no exact retained product group");
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
      fail_retained_bundle(
          "ros2_control execution product repeats an artifact role");
    }
    match = &artifact;
  }
  if (match == nullptr) {
    fail_retained_bundle(
        "ros2_control execution product omits an artifact role");
  }
  return *match;
}

[[nodiscard]] const core::RetainedArtifactRefDTO *
find_artifact(const core::RetainedProductDTO &product, std::string_view role) {
  const core::RetainedArtifactRefDTO *match = nullptr;
  for (const auto &artifact : product.artifact_refs) {
    if (artifact.artifact_role_id.value() != role) {
      continue;
    }
    if (match != nullptr) {
      fail_retained_bundle(
          "ros2_control execution product repeats an artifact role");
    }
    match = &artifact;
  }
  return match;
}

} // namespace

CanonicalValue ros2_control_execution_runtime_manifest_projection(
    const core::RuntimeUnitDTO &unit) {
  const CanonicalValue configuration =
      require_qualified_runtime_configuration(unit.configuration);
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(
           std::string(kRos2ControlExecutionRuntimeManifestApiVersion))},
      {"runtime_unit_id", CanonicalValue(unit.runtime_unit_id.value())},
      {"runtime_product_id",
       CanonicalValue(
           ros2_control_execution_runtime_product_id(unit.runtime_unit_id))},
      {"configuration", configuration},
      {"runtime_profile",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_profile_id",
            CanonicalValue(unit.runtime_profile_id.value())},
           {"runtime_profile_revision",
            CanonicalValue(unit.runtime_profile_revision.value())},
           {"implementation_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeImplementationId))},
           {"implementation_revision",
            CanonicalValue(
                ros2_control_execution_runtime_implementation_revision())},
       })},
      {"process_adapter",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
           {"process_adapter_contract_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeProcessAdapterContractId))},
           {"process_adapter_contract_revision",
            CanonicalValue(
                ros2_control_execution_runtime_process_adapter_revision())},
           {"retrieval_contract_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeRetrievalContractId))},
           {"retrieval_contract_revision",
            CanonicalValue(
                ros2_control_execution_runtime_retrieval_contract_revision())},
       })},
      {"runtime_role_mappings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"runtime_role_id",
                CanonicalValue(
                    std::string(kRos2ControlExecutionRuntimeRoleId))},
               {"implementation_role_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeImplementationId))},
           }),
       })},
      {"source_endpoint_mappings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeLogicalSceneEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeLogicalSceneEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    ros2_control_execution_runtime_logical_scene_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeStatusEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeStatusEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    ros2_control_execution_runtime_status_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
           CanonicalValue(CanonicalValue::Object{
               {"source_endpoint_role_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeProjectionEndpointRoleId))},
               {"endpoint_contract_id",
                CanonicalValue(std::string(
                    kRos2ControlExecutionRuntimeProjectionEndpointContractId))},
               {"endpoint_contract_revision",
                CanonicalValue(
                    ros2_control_execution_runtime_projection_endpoint_revision())},
               {"transport", CanonicalValue("adapter_private")},
           }),
       })},
      {"launchable_artifact",
       CanonicalValue(CanonicalValue::Object{
           {"artifact_role_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole))},
           {"media_type",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeLaunchDescriptorMediaType))},
           {"content_digest",
            CanonicalValue(sha256_digest(
                ros2_control_execution_runtime_launch_descriptor()))},
       })},
      {"platform_constraints",
       CanonicalValue(CanonicalValue::Object{
           {"operating_system",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeOperatingSystem))},
           {"launch_environment_contract",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeLaunchEnvironmentContract))},
       })},
      {"runtime_contract_offer",
       runtime_contract_offer_projection(
           require_runtime_contract_for_unit(unit).offer())},
  });
}

std::string ros2_control_execution_runtime_profile_document() {
  CanonicalValue identity(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.runtime_profile.v1")},
      {"runtime_profile_id",
       CanonicalValue(std::string(kRos2ControlExecutionRuntimeProfileId))},
      {"configuration_schema_ref",
       CanonicalValue(CanonicalValue::Object{
           {"schema_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeConfigurationSchemaId))},
           {"schema_revision",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeConfigurationSchemaRevision))},
       })},
      {"process_contract",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
       })},
      {"supervision_contract",
       CanonicalValue(CanonicalValue::Object{
           {"status_endpoint_role_id",
            CanonicalValue(std::string(
                kRos2ControlExecutionRuntimeStatusEndpointRoleId))},
       })},
      {"offered_runtime_role_ids",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(
           std::string(kRos2ControlExecutionRuntimeRoleId))})},
      {"source_endpoint_role_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(std::string(
               kRos2ControlExecutionRuntimeLogicalSceneEndpointRoleId)),
           CanonicalValue(std::string(
               kRos2ControlExecutionRuntimeProjectionEndpointRoleId)),
           CanonicalValue(std::string(
               kRos2ControlExecutionRuntimeStatusEndpointRoleId)),
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

std::string ros2_control_execution_runtime_profile_revision() {
  const auto root =
      parse_strict_json_model_yaml(
          ros2_control_execution_runtime_profile_document())
          .root();
  return require_string(
      require_member(require_object(root, "ros2_control_runtime_profile"),
                     "runtime_profile_revision",
                     "ros2_control_runtime_profile"),
      "ros2_control_runtime_profile.runtime_profile_revision");
}

bool ros2_control_execution_runtime_configuration_is_qualified(
    const core::JsonObjectDTO &configuration) noexcept {
  try {
    static_cast<void>(require_qualified_runtime_configuration(configuration));
    return true;
  } catch (...) {
    return false;
  }
}

std::string ros2_control_execution_runtime_implementation_revision() {
  return contract_revision(
      "runtime-implementation",
      "ros2-control-execution-host;retained-addressed-kernel;external-"
      "controller-observation-only;commanded-simulation-authority;"
      "jtc-follow-joint-trajectory;topic-gripper-transport;"
      "planner-scene-projection-v1;logical-scene-services-v1;"
      "build-owned-candidate-worker;runtime-contract-offer-correlation-v2");
}

std::string ros2_control_execution_runtime_process_adapter_revision() {
  return contract_revision(
      "process-adapter",
      "manager-managed;installed-target;fixed-argv;direct-child-epoch;"
      "inherited-seqpacket-channel;same-user-direct-parent;"
      "command-fenced-readiness;runtime-contract-offer-digest-echo;"
      "fixed-sibling-candidate-worker");
}

std::string ros2_control_execution_runtime_retrieval_contract_revision() {
  return contract_revision(
      "retrieval-contract",
      "retained-raw-artifact;digest-and-size-verified-launch-descriptor");
}

std::string ros2_control_execution_runtime_status_endpoint_revision() {
  return contract_revision(
      "status-endpoint",
      "adapter-private;native-execution-status-v2;direct-child-epoch;"
      "source-sequence;phase;health;product;configuration;roles;fence;"
      "runtime-contract-offer-digest");
}

std::string ros2_control_execution_runtime_projection_endpoint_revision() {
  return contract_revision(
      "projection-endpoint",
      "planner-scene-projection-rpc-v1;exact-retained-binding;bounded-"
      "framing;request-replay;health;stage;activate;retire;discard;rebuild;"
      "lease;sha256-content-identity");
}

std::string ros2_control_execution_runtime_logical_scene_endpoint_revision() {
  return contract_revision(
      "logical-scene-endpoint",
      "adapter-private;logical-scene-services-v1;reference;query;shape;"
      "compare-and-commit-edit;collision;trace;exact-revision-and-lease");
}

std::string ros2_control_execution_runtime_launch_descriptor() {
  return canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.installed_target_launch.v1")},
      {"implementation_id",
       CanonicalValue(
           std::string(kRos2ControlExecutionRuntimeImplementationId))},
      {"implementation_revision",
       CanonicalValue(
           ros2_control_execution_runtime_implementation_revision())},
      {"target_id",
       CanonicalValue(
           std::string(kRos2ControlExecutionRuntimeInstalledTargetId))},
  }));
}

std::string ros2_control_execution_runtime_product_id(
    const core::RuntimeUnitId &runtime_unit_id) {
  require_retained_portable_identity(runtime_unit_id.value(),
                                     "runtime_unit_id");
  std::string preimage("manyforge.runtime-unit-product-id.v1");
  preimage.push_back('\0');
  preimage.append(runtime_unit_id.value());
  return "runtime_unit_configuration_" + sha256_digest(preimage).substr(7U);
}

CanonicalValue parse_and_verify_ros2_control_execution_runtime_manifest(
    std::string_view canonical_bytes) {
  if (canonical_bytes.empty() ||
      canonical_bytes.size() >
          kMaxRos2ControlExecutionRuntimeManifestBytes) {
    fail_retained_bundle("ros2_control execution runtime manifest is empty or "
                         "exceeds its bound");
  }
  CanonicalValue root;
  try {
    root = parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const std::exception &error) {
    fail_retained_bundle(
        std::string("ros2_control execution runtime manifest is invalid: ") +
        error.what());
  }
  if (canonical_json(root) != canonical_bytes) {
    fail_retained_bundle(
        "ros2_control execution runtime manifest is not canonical JSON");
  }
  const auto &object =
      require_object(root, "ros2_control_execution_runtime_manifest");
  const auto &api = require_string(
      require_member(object, "api_version",
                     "ros2_control_execution_runtime_manifest"),
      "ros2_control_execution_runtime_manifest.api_version");
  const auto &runtime_unit_id = require_string(
      require_member(object, "runtime_unit_id",
                     "ros2_control_execution_runtime_manifest"),
      "ros2_control_execution_runtime_manifest.runtime_unit_id");
  require_retained_portable_identity(runtime_unit_id, "runtime_unit_id");
  const auto configuration = canonical_to_json_object(
      require_member(object, "configuration",
                     "ros2_control_execution_runtime_manifest"),
      "ros2_control_execution_runtime_manifest.configuration");
  if (!ros2_control_execution_runtime_configuration_is_qualified(
          configuration)) {
    fail_retained_bundle("ros2_control execution runtime manifest has an "
                         "unqualified configuration");
  }
  const core::RuntimeUnitDTO expected{
      core::RuntimeUnitId(runtime_unit_id),
      core::RuntimeProfileId(
          std::string(kRos2ControlExecutionRuntimeProfileId)),
      core::RuntimeProfileRevision(
          ros2_control_execution_runtime_profile_revision()),
      {},
      configuration,
  };
  const auto embedded_offer = decode_runtime_contract_offer_projection(
      require_member(object, "runtime_contract_offer",
                     "ros2_control_execution_runtime_manifest"),
      "ros2_control_execution_runtime_manifest.runtime_contract_offer");
  if (embedded_offer != require_runtime_contract_for_unit(expected).offer()) {
    fail_retained_bundle(
        "ros2_control runtime embedded offer differs from package offer");
  }
  if (api != kRos2ControlExecutionRuntimeManifestApiVersion ||
      root != ros2_control_execution_runtime_manifest_projection(expected)) {
    fail_retained_bundle("ros2_control execution runtime manifest differs "
                         "from its closed contract");
  }
  return root;
}

void require_ros2_control_execution_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes,
    std::string_view launch_descriptor_bytes,
    std::optional<std::string_view> implementation_build_identity_bytes,
    const core::RetainedBundleDTO &retained_bundle,
    const core::RuntimeUnitId &runtime_unit_id) {
  const auto manifest =
      parse_and_verify_ros2_control_execution_runtime_manifest(manifest_bytes);
  if (launch_descriptor_bytes !=
          ros2_control_execution_runtime_launch_descriptor() ||
      launch_descriptor_bytes.size() >
          kMaxRos2ControlExecutionLaunchDescriptorBytes) {
    fail_retained_bundle("ros2_control execution launch descriptor differs "
                         "from the build-owned artifact");
  }
  const auto &unit =
      require_retained_runtime_unit(retained_bundle, runtime_unit_id);
  if (unit.runtime_profile_id.value() !=
          kRos2ControlExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          ros2_control_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty() ||
      !ros2_control_execution_runtime_configuration_is_qualified(
          unit.configuration) ||
      manifest != ros2_control_execution_runtime_manifest_projection(unit)) {
    fail_retained_bundle("ros2_control execution manifest differs from its "
                         "retained graph/profile");
  }
  const auto &group =
      require_runtime_product_group(retained_bundle, runtime_unit_id);
  const auto &product = group.products.front();
  if (product.product_id.value() !=
          ros2_control_execution_runtime_product_id(runtime_unit_id) ||
      product.product_kind !=
          core::RetainedProductKind::kRuntimeUnitConfiguration ||
      product.product_contract_id.value() !=
          kRos2ControlExecutionRuntimeProductContractId ||
      product.product_contract_revision !=
          ros2_control_execution_runtime_product_contract_revision() ||
      product.artifact_refs.size() !=
          2U + static_cast<std::size_t>(
                   implementation_build_identity_bytes.has_value())) {
    fail_retained_bundle("ros2_control execution retained product has an "
                         "incoherent identity");
  }
  const auto &manifest_ref = require_artifact(
      product, kRos2ControlExecutionRuntimeManifestArtifactRole);
  const auto &launch_ref = require_artifact(
      product, kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole);
  const auto *build_identity_ref = find_artifact(
      product, kRos2ControlExecutionRuntimeBuildIdentityArtifactRole);
  if (manifest_ref.media_type !=
          kRos2ControlExecutionRuntimeManifestMediaType ||
      manifest_ref.content_digest.value() != sha256_digest(manifest_bytes) ||
      manifest_ref.byte_size != manifest_bytes.size() ||
      launch_ref.media_type !=
          kRos2ControlExecutionRuntimeLaunchDescriptorMediaType ||
      launch_ref.content_digest.value() !=
          sha256_digest(launch_descriptor_bytes) ||
      launch_ref.byte_size != launch_descriptor_bytes.size() ||
      (build_identity_ref != nullptr) !=
          implementation_build_identity_bytes.has_value()) {
    fail_retained_bundle("ros2_control execution product artifact table "
                         "differs from its bytes");
  }
  if (implementation_build_identity_bytes.has_value()) {
    const auto identity = parse_stop_implementation_build_identity(
        *implementation_build_identity_bytes);
    if (implementation_build_identity_bytes->empty() ||
        build_identity_ref->media_type !=
            kRos2ControlExecutionRuntimeBuildIdentityMediaType ||
        build_identity_ref->content_digest.value() !=
            sha256_digest(*implementation_build_identity_bytes) ||
        build_identity_ref->byte_size !=
            implementation_build_identity_bytes->size() ||
        identity.implementation_id !=
            kRos2ControlExecutionRuntimeImplementationId ||
        identity.installed_target_id !=
            kRos2ControlExecutionRuntimeInstalledTargetId) {
      fail_retained_bundle("ros2_control execution build identity differs "
                           "from its exact runtime product");
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
                fail_retained_bundle("ros2_control execution runtime profile "
                                     "source is duplicated");
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
    fail_retained_bundle("ros2_control execution product lacks exact "
                         "source/output provenance");
  }
}

} // namespace manyforge::deployment::detail

namespace manyforge::deployment::ros2_control_execution_runtime_contract {

Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id) {
  if (!verify_retained_bundle_identity(retained_bundle)) {
    throw RetainedBundleError(
        "ros2_control runtime configuration received an unverifiable bundle");
  }
  const auto &unit = detail::require_retained_runtime_unit(
      retained_bundle.value(), runtime_unit_id);
  if (unit.runtime_profile_id.value() !=
          detail::kRos2ControlExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          detail::ros2_control_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty() ||
      !detail::ros2_control_execution_runtime_configuration_is_qualified(
          unit.configuration)) {
    throw RetainedBundleError(
        "ros2_control runtime unit differs from the build-qualified profile");
  }

  const auto require_member =
      [&](const std::string_view name) -> const core::JsonValueDTO & {
    const auto found = unit.configuration.find(name);
    if (found == unit.configuration.end()) {
      throw RetainedBundleError(
          "ros2_control runtime configuration is missing " +
          std::string(name));
    }
    return found->second;
  };
  const auto require_number = [&](const std::string_view name) {
    const auto *number = std::get_if<double>(&require_member(name).value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid " +
          std::string(name));
    }
    return *number;
  };
  const auto require_uint32 = [&](const std::string_view name) {
    const double number = require_number(name);
    if (std::trunc(number) != number || number < 0.0 ||
        number >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid " +
          std::string(name));
    }
    return static_cast<std::uint32_t>(number);
  };
  const auto optional_number = [&](const std::string_view name)
      -> std::optional<double> {
    const auto found = unit.configuration.find(name);
    if (found == unit.configuration.end()) {
      return std::nullopt;
    }
    const auto *number = std::get_if<double>(&found->second.value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid " +
          std::string(name));
    }
    return *number;
  };
  const auto optional_uint32 = [&](const std::string_view name)
      -> std::optional<std::uint32_t> {
    const auto number = optional_number(name);
    if (!number.has_value()) {
      return std::nullopt;
    }
    if (std::trunc(*number) != *number || *number < 0.0 ||
        *number >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has invalid " +
          std::string(name));
    }
    return static_cast<std::uint32_t>(*number);
  };

  const auto *mode = std::get_if<std::string>(&require_member("mode").value);
  const auto *profile =
      std::get_if<std::string>(&require_member("isaac_profile").value);
  const auto *positions = std::get_if<core::JsonValueDTO::Array>(
      &require_member("initial_joint_positions_rad").value);
  if (mode == nullptr || *mode != "commanded_sim" || profile == nullptr ||
      (*profile != "lite6" && *profile != "ur10e") ||
      positions == nullptr || positions->empty()) {
    throw RetainedBundleError("ros2_control runtime configuration has an "
                              "unsupported mode, profile, or initial state");
  }

  Configuration result;
  result.isaac_profile = *profile;
  result.planner_rng_seed = require_uint32("planner_rng_seed");
  result.execution_tick_hz = require_uint32("execution_tick_hz");
  result.collision_monitor_period_ms =
      require_number("collision_monitor_period_ms");
  result.runtime_collision_immediate_query_budget_ms =
      optional_number("runtime_collision_immediate_query_budget_ms");
  result.runtime_collision_lookahead_query_budget_ms =
      optional_number("runtime_collision_lookahead_query_budget_ms");
  result.runtime_collision_lookahead_cycle_budget_ms =
      optional_number("runtime_collision_lookahead_cycle_budget_ms");
  result.runtime_collision_lookahead_s =
      optional_number("runtime_collision_lookahead_s");
  result.runtime_collision_lookahead_max_samples =
      optional_uint32("runtime_collision_lookahead_max_samples");
  result.runtime_collision_lookahead_workers =
      optional_uint32("runtime_collision_lookahead_workers");
  result.runtime_dynamic_body_max_age_cap_ms =
      optional_number("runtime_dynamic_body_max_age_cap_ms");
  result.validation_collision_workers =
      optional_uint32("validation_collision_workers");
  result.initial_joint_positions_rad.reserve(positions->size());
  for (const auto &position : *positions) {
    const auto *number = std::get_if<double>(&position.value);
    if (number == nullptr || !std::isfinite(*number)) {
      throw RetainedBundleError(
          "ros2_control runtime configuration has an invalid initial joint "
          "state");
    }
    result.initial_joint_positions_rad.push_back(*number);
  }
  if (result.execution_tick_hz == 0U ||
      !(result.collision_monitor_period_ms > 0.0) ||
      !(result.runtime_collision_immediate_query_budget_ms.value_or(50.0) <=
        result.collision_monitor_period_ms) ||
      !(result.runtime_collision_lookahead_query_budget_ms.value_or(75.0) <=
        result.runtime_collision_lookahead_cycle_budget_ms.value_or(200.0))) {
    throw RetainedBundleError(
        "ros2_control runtime configuration has invalid collision cadences");
  }
  return result;
}

std::string implementation_revision() {
  return detail::ros2_control_execution_runtime_implementation_revision();
}

} // namespace manyforge::deployment::ros2_control_execution_runtime_contract
