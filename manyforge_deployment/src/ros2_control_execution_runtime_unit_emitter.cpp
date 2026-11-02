#include "ros2_control_execution_runtime_unit_internal.hpp"

#include <algorithm>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <string>
#include <vector>

#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

[[nodiscard]] const core::RuntimeUnitDTO &require_ros2_control_runtime_unit(
    const CompilerQualifiedCellIr &compiler_ir,
    const core::RuntimeUnitId &runtime_unit_id) {
  const auto &graph = compiler_ir.applicability().composition().graph();
  const auto found =
      std::find_if(graph.runtime_units.begin(), graph.runtime_units.end(),
                   [&](const auto &unit) {
                     return unit.runtime_unit_id == runtime_unit_id;
                   });
  if (found == graph.runtime_units.end()) {
    fail_retained_bundle("ros2_control execution runtime emitter has no "
                         "exact compiler-IR subject");
  }
  if (found->runtime_profile_id.value() !=
          kRos2ControlExecutionRuntimeProfileId ||
      found->runtime_profile_revision.value() !=
          ros2_control_execution_runtime_profile_revision() ||
      !found->dependency_runtime_unit_ids.empty() ||
      !ros2_control_execution_runtime_configuration_is_qualified(
          found->configuration)) {
    fail_retained_bundle("runtime unit does not select the closed "
                         "ros2_control execution row");
  }
  const auto &profiles = compiler_ir.applicability().runtime_profiles();
  const auto profile =
      std::find_if(profiles.begin(), profiles.end(), [&](const auto &value) {
        return value.value().runtime_profile_id == found->runtime_profile_id &&
               value.value().runtime_profile_revision ==
                   found->runtime_profile_revision;
      });
  if (profile == profiles.end() ||
      profile->normalized_root() !=
          parse_strict_json_model_yaml(
              ros2_control_execution_runtime_profile_document())
              .root() ||
      profile->value().process_contract.lifecycle_control !=
          core::RuntimeLifecycleControl::kManagerManaged ||
      profile->value().offered_runtime_role_ids !=
          std::vector<core::RuntimeRoleId>{core::RuntimeRoleId(
              std::string(kRos2ControlExecutionRuntimeRoleId))} ||
      profile->value().source_endpoint_role_ids !=
          std::vector<core::EndpointRoleId>{
              core::EndpointRoleId(std::string(
                  kRos2ControlExecutionRuntimeLogicalSceneEndpointRoleId)),
              core::EndpointRoleId(std::string(
                  kRos2ControlExecutionRuntimeProjectionEndpointRoleId)),
              core::EndpointRoleId(std::string(
                  kRos2ControlExecutionRuntimeStatusEndpointRoleId))} ||
      profile->value()
              .supervision_contract.status_endpoint_role_id.value() !=
          kRos2ControlExecutionRuntimeStatusEndpointRoleId) {
    fail_retained_bundle("runtime profile does not equal the build-qualified "
                         "ros2_control execution row");
  }
  return *found;
}

} // namespace

std::string emit_ros2_control_execution_runtime_manifest(
    const CompilerQualifiedCellIr &compiler_ir,
    const core::RuntimeUnitId &runtime_unit_id) {
  const auto &unit =
      require_ros2_control_runtime_unit(compiler_ir, runtime_unit_id);
  const std::string bytes = canonical_json(
      ros2_control_execution_runtime_manifest_projection(unit));
  static_cast<void>(
      parse_and_verify_ros2_control_execution_runtime_manifest(bytes));
  return bytes;
}

void require_ros2_control_execution_runtime_manifest_matches(
    std::string_view canonical_bytes,
    const CompilerQualifiedCellIr &compiler_ir,
    const core::RuntimeUnitId &runtime_unit_id) {
  const auto parsed =
      parse_and_verify_ros2_control_execution_runtime_manifest(canonical_bytes);
  const auto &unit =
      require_ros2_control_runtime_unit(compiler_ir, runtime_unit_id);
  if (parsed != ros2_control_execution_runtime_manifest_projection(unit)) {
    fail_retained_bundle("ros2_control execution runtime manifest differs "
                         "from exact compiler IR");
  }
}

} // namespace manyforge::deployment::detail
