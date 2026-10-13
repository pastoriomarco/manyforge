#include "native_execution_runtime_unit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/runtime_contract_registry.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kMotionExecutionCapabilityId =
    "execution.trajectory.execute";
constexpr std::string_view kGripperCommandCapabilityId = "tool.gripper.command";

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

[[nodiscard]] const core::ManagerLivenessPolicyDTO&
manager_liveness_policy_value();

namespace {

[[nodiscard]] CanonicalValue
native_execution_runtime_infrastructure_projection(
    const core::RuntimeUnitId &runtime_unit_id) {
  const core::RuntimeProfileId runtime_profile_id{
      std::string(kNativeExecutionRuntimeProfileId)};
  const core::RuntimeProfileRevision runtime_profile_revision(
      native_execution_runtime_profile_revision());
  const core::ProductContractId product_contract_id{
      std::string(kNativeExecutionRuntimeProductContractId)};
  const auto product_contract_revision =
      native_execution_runtime_product_contract_revision();
  const auto &contract = require_runtime_contract(
      runtime_profile_id, runtime_profile_revision, product_contract_id,
      product_contract_revision);
  const auto& liveness = manager_liveness_policy_value();
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kNativeExecutionRuntimeManifestApiVersion))},
      {"runtime_unit_id", CanonicalValue(runtime_unit_id.value())},
      {"runtime_product_id", CanonicalValue(native_execution_runtime_product_id(
                                 runtime_unit_id))},
      {"configuration", CanonicalValue(CanonicalValue::Object{})},
      {"manager_liveness_policy",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_policy_id",
            CanonicalValue(liveness.runtime_policy_id.value())},
           {"runtime_policy_revision",
            CanonicalValue(liveness.runtime_policy_revision.value())},
           {"heartbeat_period_ns",
            CanonicalValue(
                static_cast<double>(liveness.heartbeat_period_ns))},
           {"expiry_timeout_ns",
            CanonicalValue(static_cast<double>(liveness.expiry_timeout_ns))},
           {"local_safe_state_contract_id",
            CanonicalValue(liveness.local_safe_state_contract_id.value())},
           {"local_safe_state_contract_revision",
            CanonicalValue(
                liveness.local_safe_state_contract_revision.value())},
       })},
      {"runtime_profile",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_profile_id",
            CanonicalValue(runtime_profile_id.value())},
           {"runtime_profile_revision",
            CanonicalValue(runtime_profile_revision.value())},
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
      {"runtime_contract_offer",
       runtime_contract_offer_projection(contract.offer())},
  });
}

} // namespace

CanonicalValue
native_execution_runtime_manifest_projection(const core::RuntimeUnitDTO &unit) {
  static_cast<void>(require_qualified_runtime_configuration(unit.configuration));
  if (unit.runtime_profile_id.value() != kNativeExecutionRuntimeProfileId ||
      unit.runtime_profile_revision.value() !=
          native_execution_runtime_profile_revision() ||
      !unit.dependency_runtime_unit_ids.empty()) {
    fail_retained_bundle(
        "runtime unit differs from the stable native execution profile");
  }
  return native_execution_runtime_infrastructure_projection(
      unit.runtime_unit_id);
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
  // A pure function of build constants: derive it once per process.
  static const std::string revision = [] {
    const auto root = parse_strict_json_model_yaml(
                          native_execution_runtime_profile_document())
                          .root();
    return require_string(
        require_member(
            require_object(root, "native_execution_runtime_profile"),
            "runtime_profile_revision", "native_execution_runtime_profile"),
        "native_execution_runtime_profile.runtime_profile_revision");
  }();
  return revision;
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

const core::ManagerLivenessPolicyDTO& manager_liveness_policy_value() {
  static const core::ManagerLivenessPolicyDTO policy{
      core::RuntimePolicyId(std::string(
          native_execution_runtime_contract::kManagerLivenessPolicyId)),
      core::RuntimePolicyRevision(contract_revision(
          "manager-liveness-policy",
          "retained-native-v1;heartbeat-period-ns=250000000;"
          "expiry-timeout-ns=1000000000;"
          "local-safe=manyforge.native_execution.manager_loss_safe")),
      250'000'000,
      1'000'000'000,
      core::ProductContractId(std::string(
          native_execution_runtime_contract::kManagerLivenessLocalSafeContractId)),
      core::ProductContractRevision(contract_revision(
          "manager-loss-local-safe",
          "native-kernel-direct-fail-safe;full-stop;loss-inhibit-v1"))};
  return policy;
}

std::string native_execution_runtime_implementation_revision() {
  return contract_revision(
      "runtime-implementation",
      "native-execution-host;deployment-bound-startup;operation-bound-program;"
      "projection-handoff-v1;private-rpc-v1;runtime-manifest-v2;"
      "stable-host-infrastructure;composition-owned-controller-configuration;"
      "retained-runtime-config-v2;"
      "logical-scene-services-v1;manager-managed-product-composition;"
      "build-owned-candidate-worker;sha256-projection-content;"
      "atomic-resource-table-admission;"
      "resource-derived-active-configurations;"
      "runtime-contract-offer-correlation-v2;"
      "retained-manager-liveness-receiver-v2;"
      "isolated-manager-liveness-transport-v1");
}

std::string native_execution_runtime_process_adapter_revision() {
  return contract_revision(
      "process-adapter",
      "manager-managed;installed-target;fixed-argv;direct-child-epoch;"
      "dual-inherited-seqpacket-channels;same-user-direct-parent;"
      "command-fenced-readiness;runtime-contract-offer-digest-echo;"
      "fixed-sibling-candidate-worker;isolated-manager-liveness-rpc-v1");
}

std::string native_execution_runtime_retrieval_contract_revision() {
  return contract_revision(
      "retrieval-contract",
      "retained-raw-artifact;digest-and-size-verified-launch-descriptor");
}

std::string native_execution_runtime_status_endpoint_revision() {
  return contract_revision(
      "status-endpoint",
      "adapter-private;native-execution-status-v2;direct-child-epoch;"
      "source-sequence;phase;health;product;configuration;roles;fence;"
      "runtime-contract-offer-digest");
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
  if (require_member(object, "configuration",
                     "native_execution_runtime_manifest") !=
      CanonicalValue(CanonicalValue::Object{})) {
    fail_retained_bundle(
        "native execution runtime manifest configuration must be empty");
  }
  if (api != kNativeExecutionRuntimeManifestApiVersion ||
      root != native_execution_runtime_infrastructure_projection(
                  core::RuntimeUnitId(runtime_unit_id))) {
    fail_retained_bundle(
        "native execution runtime manifest differs from its closed contract");
  }
  return root;
}

void require_native_execution_runtime_product_matches_retained_bundle(
    std::string_view manifest_bytes, std::string_view launch_descriptor_bytes,
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
      product.artifact_refs.size() != 2U) {
    fail_retained_bundle(
        "native execution retained product has an incoherent identity");
  }
  const auto &manifest_ref =
      require_artifact(product, kNativeExecutionRuntimeManifestArtifactRole);
  const auto &launch_ref = require_artifact(
      product, kNativeExecutionRuntimeLaunchDescriptorArtifactRole);
  if (manifest_ref.media_type != kNativeExecutionRuntimeManifestMediaType ||
      manifest_ref.content_digest.value() != sha256_digest(manifest_bytes) ||
      manifest_ref.byte_size != manifest_bytes.size() ||
      launch_ref.media_type !=
          kNativeExecutionRuntimeLaunchDescriptorMediaType ||
      launch_ref.content_digest.value() != sha256_digest(launch_descriptor_bytes) ||
      launch_ref.byte_size != launch_descriptor_bytes.size()) {
    fail_retained_bundle(
        "native execution product artifact table differs from its bytes");
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

std::vector<native_execution_runtime_contract::Configuration::MotionTargetLimit>
decode_execution_motion_target_limits(
    const core::JsonValueDTO::Array &motion_target_limits,
    const bool include_ordinary_jerk) {
  using native_execution_runtime_contract::Configuration;
  std::vector<Configuration::MotionTargetLimit> result;
  result.reserve(motion_target_limits.size());
  for (const auto &entry : motion_target_limits) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&entry.value);
    if (object == nullptr ||
        object->size() != (include_ordinary_jerk ? 4U : 3U)) {
      throw RetainedBundleError(
          "execution runtime configuration has an invalid motion target limit");
    }
    const auto component = object->find("controllable_component_id");
    const auto target = object->find("command_target_id");
    const auto limit = object->find("maximum_tcp_linear_speed_mps");
    if (component == object->end() || target == object->end() ||
        limit == object->end()) {
      throw RetainedBundleError("execution runtime configuration has an "
                                "incomplete motion target limit");
    }
    const auto *component_id =
        std::get_if<std::string>(&component->second.value);
    const auto *target_id = std::get_if<std::string>(&target->second.value);
    const auto *maximum = std::get_if<double>(&limit->second.value);
    if (component_id == nullptr || component_id->empty() ||
        target_id == nullptr || target_id->empty() || maximum == nullptr ||
        !std::isfinite(*maximum) || *maximum <= 0.0 || *maximum > 1000.0) {
      throw RetainedBundleError(
          "execution runtime configuration has an invalid "
          "motion target limit value");
    }
    result.push_back(Configuration::MotionTargetLimit{
        core::ControllableComponentId(*component_id),
        core::CommandTargetId(*target_id), *maximum,
        include_ordinary_jerk
            ? detail::decode_ordinary_motion_jerk_settings(
                  std::get<core::JsonObjectDTO>(
                      object->at("ordinary_motion_jerk").value))
            : native_execution_runtime_contract::OrdinaryMotionJerkSettings{}});
  }
  std::sort(result.begin(), result.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id);
            });
  if (std::adjacent_find(result.begin(), result.end(),
                         [](const auto &left, const auto &right) {
                           return left.controllable_component_id ==
                                      right.controllable_component_id &&
                                  left.command_target_id ==
                                      right.command_target_id;
                         }) != result.end()) {
    throw RetainedBundleError(
        "execution runtime configuration repeats a motion target limit");
  }
  return result;
}

std::vector<
    native_execution_runtime_contract::Configuration::GripperTargetConfiguration>
decode_execution_gripper_target_configurations(
    const core::JsonValueDTO::Array &gripper_target_configurations,
    const std::string_view label) {
  using native_execution_runtime_contract::Configuration;
  std::vector<Configuration::GripperTargetConfiguration> result;
  result.reserve(
      gripper_target_configurations.size());
  for (const auto &entry : gripper_target_configurations) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&entry.value);
    if (object == nullptr || (object->size() != 12U && object->size() != 13U)) {
      throw RetainedBundleError(
          std::string(label) + " has an invalid gripper target configuration");
    }
    const auto string_member = [&](const std::string_view name) -> std::string {
      const auto found = object->find(name);
      if (found == object->end()) {
        throw RetainedBundleError(
            std::string(label) + " has an incomplete gripper target configuration");
      }
      const auto *value = std::get_if<std::string>(&found->second.value);
      if (value == nullptr || value->empty()) {
        throw RetainedBundleError(
            std::string(label) + " has an invalid gripper target string");
      }
      return *value;
    };
    const auto string_array_member =
        [&](const std::string_view name, const bool require_nonempty) {
          const auto found = object->find(name);
          const auto *values = found == object->end()
                                   ? nullptr
                                   : std::get_if<core::JsonValueDTO::Array>(
                                         &found->second.value);
          if (values == nullptr || (require_nonempty && values->empty())) {
            throw RetainedBundleError(
                std::string(label) + " has an invalid gripper target list");
          }
          std::vector<std::string> result_values;
          result_values.reserve(values->size());
          for (const auto &value : *values) {
            const auto *text = std::get_if<std::string>(&value.value);
            if (text == nullptr || text->empty()) {
              throw RetainedBundleError(
                  std::string(label) + " has an invalid gripper target list item");
            }
            result_values.push_back(*text);
          }
          if (!std::is_sorted(result_values.begin(), result_values.end()) ||
              std::adjacent_find(result_values.begin(), result_values.end()) !=
                  result_values.end()) {
            throw RetainedBundleError(
                std::string(label) + " has a noncanonical gripper target list");
          }
          return result_values;
        };
    const auto duration = object->find("command_duration_s");
    const auto *duration_s =
        duration == object->end()
            ? nullptr
            : std::get_if<double>(&duration->second.value);
    const auto component_id = string_member("controllable_component_id");
    const auto target_id = string_member("command_target_id");
    const auto actuation_kind = string_member("actuation_kind");
    if (object->size() != 12U) {
      throw RetainedBundleError(
          std::string(label) + " has an unknown gripper target field");
    }
    if (duration_s == nullptr || !std::isfinite(*duration_s) ||
        *duration_s <= 0.0 || *duration_s > 60.0 ||
        (actuation_kind != "position_commanded" &&
         actuation_kind != "pneumatic")) {
      throw RetainedBundleError(
          std::string(label) + " has invalid gripper target semantics");
    }
    result.push_back(Configuration::GripperTargetConfiguration{
        core::ControllableComponentId(component_id), core::CommandTargetId(target_id),
        string_member("description"), actuation_kind, string_member("open_state_ref"),
        string_member("closed_state_ref"), *duration_s, string_array_member("touch_links", true),
        string_member("mount_frame"), string_member("attach_frame"), string_member("tcp_frame"),
        string_array_member("auxiliary_frames", false)});
  }
  std::sort(result.begin(),
            result.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id);
            });
  if (std::adjacent_find(
          result.begin(),
          result.end(),
          [](const auto &left, const auto &right) {
            return left.controllable_component_id ==
                       right.controllable_component_id &&
                   left.command_target_id == right.command_target_id;
          }) != result.end()) {
    throw RetainedBundleError(
        std::string(label) + " repeats a gripper target configuration");
  }
  return result;
}

native_execution_runtime_contract::Configuration
decode_native_execution_runtime_configuration(
    const core::JsonObjectDTO &configuration, const core::CellGraphDTO &graph,
    const core::RuntimeUnitId &runtime_unit_id) {
  using native_execution_runtime_contract::Configuration;
  const auto require_member =
      [&](const std::string_view name) -> const core::JsonValueDTO & {
    const auto found = configuration.find(name);
    if (found == configuration.end()) {
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
  const auto *motion_target_limits = std::get_if<core::JsonValueDTO::Array>(
      &require_member("motion_target_limits").value);
  const auto *direct_control_joint_limits =
      std::get_if<core::JsonValueDTO::Array>(
          &require_member("direct_control_joint_limits").value);
  const auto gripper_configurations_member =
      configuration.find("gripper_target_configurations");
  const auto *gripper_target_configurations =
      gripper_configurations_member == configuration.end()
          ? nullptr
          : std::get_if<core::JsonValueDTO::Array>(
                &gripper_configurations_member->second.value);
  // Motion-only fields are required exactly when this unit hosts a motion
  // capability (H1a-IF.2). A capability-only unit (for example gripper-only)
  // carries them empty; placeholder joints or TCP limits are refused.
  const auto hosts = [&](const core::ControllableComponentId &component,
                         const core::CommandTargetId &target,
                         const std::string_view capability) {
    return std::any_of(
        graph.control_bindings.begin(), graph.control_bindings.end(),
        [&](const auto &binding) {
          return binding.provider_runtime_unit_id == runtime_unit_id &&
                 binding.capability_id.value() == capability &&
                 (component.empty() ||
                  (binding.controllable_component_id == component &&
                   binding.command_target_id == target));
        });
  };
  const bool motion_advertised = std::any_of(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [&](const auto &binding) {
        return binding.provider_runtime_unit_id == runtime_unit_id &&
               binding.capability_id.value() == kMotionExecutionCapabilityId;
      });
  if (mode == nullptr || *mode != "simulated" || positions == nullptr ||
      motion_target_limits == nullptr || direct_control_joint_limits == nullptr ||
      positions->empty() != !motion_advertised ||
      motion_target_limits->empty() != !motion_advertised ||
      (!motion_advertised && !direct_control_joint_limits->empty())) {
    throw RetainedBundleError(
        "native runtime configuration has an unsupported mode or initial "
        "state for its advertised capabilities");
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
  result.motion_target_limits =
      decode_execution_motion_target_limits(*motion_target_limits, true);
  result.direct_control_joint_limits.reserve(
      direct_control_joint_limits->size());
  for (const auto &entry : *direct_control_joint_limits) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&entry.value);
    if (object == nullptr || object->size() < 4U || object->size() > 6U) {
      throw RetainedBundleError(
          "native runtime configuration has an invalid direct-control joint limit");
    }
    const auto component = object->find("controllable_component_id");
    const auto target = object->find("command_target_id");
    const auto joint = object->find("source_joint_name");
    if (component == object->end() || target == object->end() ||
        joint == object->end()) {
      throw RetainedBundleError(
          "native runtime configuration has an incomplete direct-control joint limit");
    }
    const auto *component_id =
        std::get_if<std::string>(&component->second.value);
    const auto *target_id = std::get_if<std::string>(&target->second.value);
    const auto *source_joint_name =
        std::get_if<std::string>(&joint->second.value);
    if (component_id == nullptr || component_id->empty() ||
        target_id == nullptr || target_id->empty() ||
        source_joint_name == nullptr || source_joint_name->empty()) {
      throw RetainedBundleError(
          "native runtime configuration has an invalid direct-control joint identity");
    }
    const auto optional_positive = [&](const std::string_view name) {
      const auto found = object->find(name);
      if (found == object->end()) {
        return std::optional<double>{};
      }
      const auto *value = std::get_if<double>(&found->second.value);
      if (value == nullptr || !std::isfinite(*value) || *value <= 0.0) {
        throw RetainedBundleError(
            "native runtime configuration has an invalid direct-control joint limit value");
      }
      return std::optional<double>{*value};
    };
    const auto velocity = optional_positive("max_velocity_rad_s");
    const auto acceleration = optional_positive("max_acceleration_rad_s2");
    const auto jerk = optional_positive("max_jerk_rad_s3");
    if (!velocity.has_value() && !acceleration.has_value() &&
        !jerk.has_value()) {
      throw RetainedBundleError(
          "native runtime configuration has an empty direct-control joint limit");
    }
    result.direct_control_joint_limits.push_back(
        Configuration::DirectControlJointLimit{
            core::ControllableComponentId(*component_id),
            core::CommandTargetId(*target_id), *source_joint_name, velocity,
            acceleration, jerk});
  }
  std::sort(result.direct_control_joint_limits.begin(),
            result.direct_control_joint_limits.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id,
                              left.source_joint_name) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id,
                              right.source_joint_name);
            });
  if (std::adjacent_find(
          result.direct_control_joint_limits.begin(),
          result.direct_control_joint_limits.end(),
          [](const auto &left, const auto &right) {
            return left.controllable_component_id ==
                       right.controllable_component_id &&
                   left.command_target_id == right.command_target_id &&
                   left.source_joint_name == right.source_joint_name;
          }) != result.direct_control_joint_limits.end()) {
    throw RetainedBundleError(
        "native runtime configuration repeats a direct-control joint limit");
  }
  // Direct-control limits apply only to exact motion-execution targets hosted
  // by this unit; they cannot qualify a gripper or foreign component target.
  for (const auto &limit : result.direct_control_joint_limits) {
    if (!hosts(limit.controllable_component_id, limit.command_target_id,
               kMotionExecutionCapabilityId)) {
      throw RetainedBundleError(
          "native runtime direct-control limit names a target this unit does "
          "not host for motion execution");
    }
  }
  // Each motion limit names a motion target this unit actually hosts.
  for (const auto &limit : result.motion_target_limits) {
    if (!hosts(limit.controllable_component_id, limit.command_target_id,
               kMotionExecutionCapabilityId)) {
      throw RetainedBundleError(
          "native runtime motion limit names a target this unit does not host");
    }
  }
  if (gripper_configurations_member != configuration.end() &&
      gripper_target_configurations == nullptr) {
    throw RetainedBundleError(
        "native runtime configuration has invalid gripper target configurations");
  }
  if (gripper_target_configurations != nullptr) {
    result.gripper_target_configurations =
        decode_execution_gripper_target_configurations(
            *gripper_target_configurations, "native runtime configuration");
  }
  // Each gripper configuration names a gripper target this unit hosts, on
  // its own component; it never borrows an arm's identity.
  for (const auto &gripper : result.gripper_target_configurations) {
    if (!hosts(gripper.controllable_component_id, gripper.command_target_id,
               kGripperCommandCapabilityId)) {
      throw RetainedBundleError(
          "native runtime gripper configuration names a target this unit does "
          "not host");
    }
  }
  if (result.execution_tick_hz == 0U ||
      !(result.collision_monitor_period_ms > 0.0)) {
    throw RetainedBundleError(
        "native runtime configuration has an invalid cadence");
  }
  return result;
}

} // namespace manyforge::deployment::detail

namespace manyforge::deployment::native_execution_runtime_contract {

CanonicalValue empty_host_configuration() {
  return CanonicalValue(CanonicalValue::Object{
      {"mode", CanonicalValue("simulated")},
      {"planner_rng_seed", CanonicalValue(12345.0)},
      {"execution_tick_hz", CanonicalValue(100.0)},
      {"collision_monitor_period_ms", CanonicalValue(50.0)},
      {"initial_joint_positions_rad", CanonicalValue(CanonicalValue::Array{})},
      {"motion_target_limits", CanonicalValue(CanonicalValue::Array{})},
      {"direct_control_joint_limits", CanonicalValue(CanonicalValue::Array{})},
      {"gripper_target_configurations", CanonicalValue(CanonicalValue::Array{})},
  });
}

CanonicalValue empty_host_deployment(
    const core::CellId &cell_id, const CanonicalValue &runtime_profile_reference,
    std::string_view deployment_id, std::string_view name) {
  using Object = CanonicalValue::Object;
  using Array = CanonicalValue::Array;
  const auto *profile = runtime_profile_reference.find_object_member("record_id");
  const auto *revision = runtime_profile_reference.find_object_member("record_revision");
  if (profile == nullptr || revision == nullptr || !profile->is_string() ||
      profile->as_string() != kRuntimeProfileId || !revision->is_string()) {
    throw RetainedBundleError("empty native Deployment requires its qualified runtime profile");
  }
  return CanonicalValue(Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id", CanonicalValue(std::string(deployment_id))},
      {"deployment_revision", CanonicalValue("sha256:" + std::string(64U, '0'))},
      {"cell_id", CanonicalValue(cell_id.value())},
      {"name", CanonicalValue(std::string(name))},
      {"catalogs", CanonicalValue(Object{{"records", CanonicalValue(Array{runtime_profile_reference})}})},
      {"graph", CanonicalValue(Object{
          {"runtime_units", CanonicalValue(Array{CanonicalValue(Object{
              {"runtime_unit_id", CanonicalValue(std::string(kCellHostRuntimeUnitId))},
              {"runtime_profile_id", *profile},
              {"runtime_profile_revision", *revision},
              {"dependency_runtime_unit_ids", CanonicalValue(Array{})},
              {"configuration", empty_host_configuration()},
          })})},
          {"model_instances", CanonicalValue(Array{})},
          {"controllable_components", CanonicalValue(Array{})},
          {"control_domains", CanonicalValue(Array{})},
          {"control_bindings", CanonicalValue(Array{})},
          {"component_attachments", CanonicalValue(Array{})},
      })},
      {"desired_control_configurations", CanonicalValue(Array{})},
      {"capability_provider_bindings", CanonicalValue(Array{})},
  });
}

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

  return detail::decode_native_execution_runtime_configuration(
      unit.configuration, retained_bundle.value().cell_graph, runtime_unit_id);
}

std::string implementation_revision() {
  return detail::native_execution_runtime_implementation_revision();
}

const core::ManagerLivenessPolicyDTO& manager_liveness_policy() {
  return detail::manager_liveness_policy_value();
}

} // namespace manyforge::deployment::native_execution_runtime_contract
