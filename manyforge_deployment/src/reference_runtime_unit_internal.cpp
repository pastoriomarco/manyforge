#include "reference_runtime_unit_internal.hpp"

#include <algorithm>
#include <string>
#include <string_view>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/cell_compiler.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

[[nodiscard]] const core::RuntimeUnitDTO& require_reference_runtime_unit(
    const CompilerQualifiedCellIr& compiler_ir, const core::RuntimeUnitId& runtime_unit_id) {
  const auto& graph = compiler_ir.applicability().composition().graph();
  const auto found =
      std::find_if(graph.runtime_units.begin(), graph.runtime_units.end(),
                   [&](const auto& unit) { return unit.runtime_unit_id == runtime_unit_id; });
  if (found == graph.runtime_units.end()) {
    fail_retained_bundle("reference runtime emitter has no exact compiler-IR subject");
  }
  if (found->runtime_unit_id.value() != kReferenceRuntimeUnitId ||
      found->runtime_profile_id.value() != kReferenceRuntimeProfileId ||
      found->runtime_profile_revision.value() != reference_runtime_profile_revision() ||
      !found->dependency_runtime_unit_ids.empty() || !found->configuration.empty()) {
    fail_retained_bundle("runtime unit does not select the closed reference runtime row");
  }
  const auto& profiles = compiler_ir.applicability().runtime_profiles();
  const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const auto& candidate) {
    return candidate.value().runtime_profile_id == found->runtime_profile_id &&
           candidate.value().runtime_profile_revision == found->runtime_profile_revision;
  });
  if (profile == profiles.end() ||
      profile->normalized_root() !=
          parse_strict_json_model_yaml(reference_runtime_profile_document()).root() ||
      profile->value().process_contract.lifecycle_control !=
          core::RuntimeLifecycleControl::kManagerManaged ||
      profile->value().offered_runtime_role_ids.size() != 1U ||
      profile->value().offered_runtime_role_ids.front().value() != kReferenceRuntimeRoleId ||
      profile->value().source_endpoint_role_ids.size() != 1U ||
      profile->value().source_endpoint_role_ids.front().value() !=
          kReferenceRuntimeStatusEndpointRoleId ||
      profile->value().supervision_contract.status_endpoint_role_id.value() !=
          kReferenceRuntimeStatusEndpointRoleId) {
    fail_retained_bundle("runtime profile does not equal the build-qualified reference row");
  }
  return *found;
}

[[nodiscard]] CanonicalValue reference_runtime_projection(
    const CompilerQualifiedCellIr& compiler_ir, const core::RuntimeUnitId& runtime_unit_id) {
  const auto& unit = require_reference_runtime_unit(compiler_ir, runtime_unit_id);
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kReferenceRuntimeManifestApiVersion))},
      {"runtime_unit_id", CanonicalValue(unit.runtime_unit_id.value())},
      {"runtime_product_id", CanonicalValue(reference_runtime_product_id(unit.runtime_unit_id))},
      {"runtime_profile",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_profile_id", CanonicalValue(unit.runtime_profile_id.value())},
           {"runtime_profile_revision", CanonicalValue(unit.runtime_profile_revision.value())},
           {"implementation_id", CanonicalValue(std::string(kReferenceRuntimeImplementationId))},
           {"implementation_revision", CanonicalValue(reference_runtime_implementation_revision())},
       })},
      {"process_adapter",
       CanonicalValue(CanonicalValue::Object{
           {"lifecycle_control", CanonicalValue("manager_managed")},
           {"process_adapter_contract_id",
            CanonicalValue(std::string(kReferenceRuntimeProcessAdapterContractId))},
           {"process_adapter_contract_revision",
            CanonicalValue(reference_runtime_process_adapter_revision())},
           {"retrieval_contract_id",
            CanonicalValue(std::string(kReferenceRuntimeRetrievalContractId))},
           {"retrieval_contract_revision",
            CanonicalValue(reference_runtime_retrieval_contract_revision())},
       })},
      {"runtime_role_mappings",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
           {"runtime_role_id", CanonicalValue(std::string(kReferenceRuntimeRoleId))},
           {"implementation_role_id",
            CanonicalValue(std::string(kReferenceRuntimeImplementationId))},
       })})},
      {"source_endpoint_mappings",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
           {"source_endpoint_role_id",
            CanonicalValue(std::string(kReferenceRuntimeStatusEndpointRoleId))},
           {"endpoint_contract_id",
            CanonicalValue(std::string(kReferenceRuntimeStatusEndpointContractId))},
           {"endpoint_contract_revision",
            CanonicalValue(reference_runtime_status_endpoint_revision())},
           {"transport", CanonicalValue("stdout_line")},
       })})},
      {"launchable_artifact",
       CanonicalValue(CanonicalValue::Object{
           {"artifact_role_id",
            CanonicalValue(std::string(kReferenceRuntimeLaunchableArtifactRole))},
           {"media_type", CanonicalValue(std::string(kReferenceRuntimeLaunchableMediaType))},
           {"content_digest", CanonicalValue(sha256_digest(reference_runtime_launchable_script()))},
       })},
      {"platform_constraints",
       CanonicalValue(CanonicalValue::Object{
           {"operating_system", CanonicalValue(std::string(kReferenceRuntimeOperatingSystem))},
           {"shell_contract", CanonicalValue(std::string(kReferenceRuntimeShellContract))},
       })},
  });
}

}  // namespace

std::string emit_reference_runtime_manifest(const CompilerQualifiedCellIr& compiler_ir,
                                            const core::RuntimeUnitId& runtime_unit_id) {
  const std::string bytes =
      canonical_json(reference_runtime_projection(compiler_ir, runtime_unit_id));
  static_cast<void>(parse_and_verify_reference_runtime_manifest(bytes));
  return bytes;
}

void require_reference_runtime_manifest_matches(std::string_view canonical_bytes,
                                                const CompilerQualifiedCellIr& compiler_ir,
                                                const core::RuntimeUnitId& runtime_unit_id) {
  const CanonicalValue parsed = parse_and_verify_reference_runtime_manifest(canonical_bytes);
  if (parsed != reference_runtime_projection(compiler_ir, runtime_unit_id)) {
    fail_retained_bundle("reference runtime manifest differs from exact compiler IR");
  }
}

}  // namespace manyforge::deployment::detail
