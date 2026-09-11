#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_core/resolved_stop_base_plan_codec.hpp>

#include "control_domain_control_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw RetainedBundleError("resolved Stop base-plan composition: " +
                            std::move(message));
}

template <typename Value>
[[nodiscard]] core::Result<Value> readiness_error(std::string reason,
                                                  std::string message) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

[[nodiscard]] const CanonicalValue &member(const CanonicalValue &value,
                                           std::string_view name,
                                           std::string_view path) {
  if (!value.is_object()) {
    fail(std::string(path) + " is not an object");
  }
  const auto *result = value.find_object_member(name);
  if (result == nullptr) {
    fail(std::string(path) + " omits " + std::string(name));
  }
  return *result;
}

[[nodiscard]] const CanonicalValue::Array &array(const CanonicalValue &value,
                                                 std::string_view path) {
  if (!value.is_array()) {
    fail(std::string(path) + " is not an array");
  }
  return value.as_array();
}

[[nodiscard]] const std::string &text(const CanonicalValue &value,
                                      std::string_view path) {
  if (!value.is_string()) {
    fail(std::string(path) + " is not a string");
  }
  return value.as_string();
}

[[nodiscard]] std::int64_t integer(const CanonicalValue &value,
                                   std::string_view path) {
  if (!value.is_number()) {
    fail(std::string(path) + " is not a number");
  }
  const double number = value.as_number();
  if (!std::isfinite(number) || std::trunc(number) != number ||
      number < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
      number > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    fail(std::string(path) + " is not an exact signed integer");
  }
  return static_cast<std::int64_t>(number);
}

[[nodiscard]] std::uint64_t unsigned_integer(const CanonicalValue &value,
                                             std::string_view path) {
  const auto number = integer(value, path);
  if (number < 0) {
    fail(std::string(path) + " is negative");
  }
  return static_cast<std::uint64_t>(number);
}

[[nodiscard]] bool is_null(const CanonicalValue &value) noexcept {
  return value.is_null();
}

template <typename Strong>
[[nodiscard]] std::optional<Strong> optional_strong(const CanonicalValue &value,
                                                    std::string_view path) {
  if (is_null(value)) {
    return std::nullopt;
  }
  return Strong(text(value, path));
}

[[nodiscard]] core::ProviderContractRefDTO
parse_contract(const CanonicalValue &value, std::string_view path) {
  return {
      core::ProviderContractRoleId(
          text(member(value, "provider_contract_role_id", path), path)),
      core::SchemaId(text(member(value, "contract_schema_id", path), path)),
      core::SchemaRevision(
          text(member(value, "contract_schema_revision", path), path)),
  };
}

[[nodiscard]] std::optional<core::ProviderContractRefDTO>
optional_contract(const CanonicalValue &value, std::string_view path) {
  if (is_null(value)) {
    return std::nullopt;
  }
  return parse_contract(value, path);
}

[[nodiscard]] core::RetainedArtifactRefDTO
parse_artifact(const CanonicalValue &value, std::string_view path) {
  return {
      core::ArtifactRoleId(text(member(value, "artifact_role_id", path), path)),
      text(member(value, "media_type", path), path),
      unsigned_integer(member(value, "byte_size", path), path),
      core::ContentDigest(text(member(value, "content_digest", path), path)),
      core::ContentAddressedStoreKey(
          text(member(value, "content_addressed_store_key", path), path)),
  };
}

[[nodiscard]] core::JsonValueDTO parse_json_value(const CanonicalValue &value,
                                                  std::size_t depth = 0U) {
  if (depth > 32U) {
    fail("default contract parameters exceed their depth bound");
  }
  core::JsonValueDTO result;
  if (value.is_null()) {
    result.value = nullptr;
  } else if (value.is_bool()) {
    result.value = value.as_bool();
  } else if (value.is_number()) {
    result.value = value.as_number();
  } else if (value.is_string()) {
    result.value = value.as_string();
  } else if (value.is_array()) {
    core::JsonValueDTO::Array items;
    items.reserve(value.as_array().size());
    for (const auto &item : value.as_array()) {
      items.push_back(parse_json_value(item, depth + 1U));
    }
    result.value = std::move(items);
  } else {
    core::JsonObjectDTO object;
    for (const auto &[name, item] : value.as_object()) {
      object.emplace(name, parse_json_value(item, depth + 1U));
    }
    result.value = std::move(object);
  }
  return result;
}

[[nodiscard]] core::JsonObjectDTO parse_json_object(const CanonicalValue &value,
                                                    std::string_view path) {
  if (!value.is_object()) {
    fail(std::string(path) + " is not an object");
  }
  auto parsed = parse_json_value(value);
  return std::get<core::JsonObjectDTO>(std::move(parsed.value));
}

[[nodiscard]] core::ContainmentGranularity
parse_granularity(std::string_view value) {
  if (value == "resource") {
    return core::ContainmentGranularity::kResource;
  }
  if (value == "controller_set") {
    return core::ContainmentGranularity::kControllerSet;
  }
  if (value == "control_domain") {
    return core::ContainmentGranularity::kControlDomain;
  }
  fail("containment granularity is unknown");
}

[[nodiscard]] core::StopEscalationStageKind
parse_stage_kind(std::string_view value) {
  if (value == "nominal") {
    return core::StopEscalationStageKind::kNominal;
  }
  if (value == "controlled_fallback") {
    return core::StopEscalationStageKind::kControlledFallback;
  }
  if (value == "software_hard") {
    return core::StopEscalationStageKind::kSoftwareHard;
  }
  if (value == "safety_system_request") {
    return core::StopEscalationStageKind::kSafetySystemRequest;
  }
  fail("Stop escalation stage kind is unknown");
}

[[nodiscard]] core::StopContainmentCommandKind
parse_command_kind(std::string_view value) {
  if (value == "controller_goal_cancel") {
    return core::StopContainmentCommandKind::kControllerGoalCancel;
  }
  if (value == "motion_controlled_stop") {
    return core::StopContainmentCommandKind::kMotionControlledStop;
  }
  if (value == "capability_command_cancel") {
    return core::StopContainmentCommandKind::kCapabilityCommandCancel;
  }
  if (value == "safe_output_command") {
    return core::StopContainmentCommandKind::kSafeOutputCommand;
  }
  if (value == "logical_cancel") {
    return core::StopContainmentCommandKind::kLogicalCancel;
  }
  if (value == "registered") {
    return core::StopContainmentCommandKind::kRegistered;
  }
  fail("Stop containment command kind is unknown");
}

[[nodiscard]] core::StopEvidenceRequirementKind
parse_evidence_kind(std::string_view value) {
  if (value == "controller_goal_terminal") {
    return core::StopEvidenceRequirementKind::kControllerGoalTerminal;
  }
  if (value == "motion_rest") {
    return core::StopEvidenceRequirementKind::kMotionRest;
  }
  if (value == "discrete_acknowledgement") {
    return core::StopEvidenceRequirementKind::kDiscreteAcknowledgement;
  }
  if (value == "io_safe_output") {
    return core::StopEvidenceRequirementKind::kIoSafeOutput;
  }
  if (value == "logical_cancellation_no_continuing_effect") {
    return core::StopEvidenceRequirementKind::
        kLogicalCancellationNoContinuingEffect;
  }
  if (value == "registered") {
    return core::StopEvidenceRequirementKind::kRegistered;
  }
  fail("Stop evidence kind is unknown");
}

[[nodiscard]] core::StopEvidenceSourceKind
parse_source_kind(std::string_view value) {
  if (value == "controller_goal_status") {
    return core::StopEvidenceSourceKind::kControllerGoalStatus;
  }
  if (value == "binding_state_authority") {
    return core::StopEvidenceSourceKind::kBindingStateAuthority;
  }
  if (value == "command_target_status") {
    return core::StopEvidenceSourceKind::kCommandTargetStatus;
  }
  if (value == "provider_evidence") {
    return core::StopEvidenceSourceKind::kProviderEvidence;
  }
  fail("Stop evidence source kind is unknown");
}

[[nodiscard]] core::StopEvidenceObservationKind
parse_observation_kind(std::string_view value) {
  if (value == "command_result") {
    return core::StopEvidenceObservationKind::kCommandResult;
  }
  if (value == "post_command_sample") {
    return core::StopEvidenceObservationKind::kPostCommandSample;
  }
  if (value == "post_command_dwell") {
    return core::StopEvidenceObservationKind::kPostCommandDwell;
  }
  fail("Stop evidence observation kind is unknown");
}

[[nodiscard]] core::StateAuthorityClass
parse_authority_class(std::string_view value) {
  if (value == "commanded") {
    return core::StateAuthorityClass::kCommanded;
  }
  if (value == "measured") {
    return core::StateAuthorityClass::kMeasured;
  }
  if (value == "predicted") {
    return core::StateAuthorityClass::kPredicted;
  }
  fail("state authority class is unknown");
}

[[nodiscard]] core::SafetySystemRequestActionClass
parse_safety_action(std::string_view value) {
  if (value == "protective_stop_request") {
    return core::SafetySystemRequestActionClass::kProtectiveStopRequest;
  }
  if (value == "emergency_stop_request") {
    return core::SafetySystemRequestActionClass::kEmergencyStopRequest;
  }
  fail("safety-system request action is unknown");
}

[[nodiscard]] core::ExternalResourceKind
parse_external_kind(std::string_view value) {
  if (value == "model_endpoint") {
    return core::ExternalResourceKind::kModelEndpoint;
  }
  if (value == "content_store") {
    return core::ExternalResourceKind::kContentStore;
  }
  if (value == "data_source") {
    return core::ExternalResourceKind::kDataSource;
  }
  if (value == "service_endpoint") {
    return core::ExternalResourceKind::kServiceEndpoint;
  }
  fail("external resource kind is unknown");
}

[[nodiscard]] core::StopContainmentCommandDTO
parse_command(const CanonicalValue &value, std::string_view path) {
  return {
      core::StopBudgetPhaseId(
          text(member(value, "command_phase_id", path), path)),
      parse_command_kind(text(member(value, "command_kind", path), path)),
      optional_contract(member(value, "provider_contract_ref", path), path),
      optional_strong<core::RegisteredStopCommandKindId>(
          member(value, "registered_command_kind_id", path), path),
  };
}

[[nodiscard]] core::StopDeadlineBudgetDTO
parse_budget(const CanonicalValue &value, std::string_view path) {
  core::StopDeadlineBudgetDTO result;
  result.outer_duration_ns =
      integer(member(value, "outer_duration_ns", path), path);
  result.kernel_completion_reserve_ns =
      integer(member(value, "kernel_completion_reserve_ns", path), path);
  for (const auto &phase : array(member(value, "phases", path), path)) {
    result.phases.push_back(
        {core::StopBudgetPhaseId(text(member(phase, "phase_id", path), path)),
         integer(member(phase, "deadline_offset_ns", path), path)});
  }
  return result;
}

[[nodiscard]] core::StopEvidenceRequirementDTO
parse_evidence(const CanonicalValue &value, std::string_view path) {
  const auto &source = member(value, "source_requirement", path);
  const auto &observation = member(value, "observation_semantics", path);
  std::vector<core::StateAuthorityClass> allowed;
  for (const auto &authority :
       array(member(source, "allowed_authority_classes", path), path)) {
    allowed.push_back(parse_authority_class(text(authority, path)));
  }
  const auto &dwell = member(observation, "minimum_dwell_ns", path);
  return {
      core::StopEvidenceItemId(
          text(member(value, "evidence_item_id", path), path)),
      core::StopBudgetPhaseId(text(member(value, "phase_id", path), path)),
      parse_evidence_kind(text(member(value, "evidence_kind", path), path)),
      optional_contract(member(value, "provider_contract_ref", path), path),
      optional_strong<core::RegisteredStopEvidenceKindId>(
          member(value, "registered_evidence_kind_id", path), path),
      core::StopEvidenceSourceRequirementDTO{
          parse_source_kind(text(member(source, "source_kind", path), path)),
          integer(member(source, "freshness_budget_ns", path), path),
          optional_strong<core::EndpointRoleId>(
              member(source, "endpoint_role_id", path), path),
          std::move(allowed)},
      core::StopEvidenceObservationDTO{
          parse_observation_kind(
              text(member(observation, "observation_kind", path), path)),
          is_null(dwell) ? std::nullopt
                         : std::optional<std::int64_t>(integer(dwell, path))},
  };
}

struct RuntimeProduct final {
  const core::RetainedProductDTO *product{};
  core::ContentDigest content_digest;
  core::RuntimeEpoch runtime_epoch;
};

[[nodiscard]] RuntimeProduct
require_runtime_product(const RetainedBundle &retained_bundle,
                        const core::RuntimeParticipantBindingDTO &binding,
                        const core::RuntimeControlPathDTO &control_path) {
  const core::RetainedRuntimeUnitProductsDTO *group = nullptr;
  for (const auto &candidate :
       retained_bundle.value().compiled_products.runtime_unit_products) {
    if (candidate.runtime_unit_id == control_path.provider_runtime_unit_id) {
      if (group != nullptr) {
        fail("retained bundle duplicates the provider runtime-unit group");
      }
      group = &candidate;
    }
  }
  if (group == nullptr || group->products.size() != 1U ||
      group->products.front().product_kind !=
          core::RetainedProductKind::kRuntimeUnitConfiguration) {
    fail("provider runtime unit has no sole qualified runtime product");
  }

  const core::RetainedOutputRecordDTO *output = nullptr;
  for (const auto &candidate :
       retained_bundle.value().provenance.output_records) {
    if (candidate.product_id == group->products.front().product_id &&
        candidate.product_kind == group->products.front().product_kind) {
      if (output != nullptr) {
        fail("retained provenance duplicates the runtime product output");
      }
      output = &candidate;
    }
  }
  const core::RuntimeUnitEpochBindingDTO *epoch = nullptr;
  for (const auto &candidate : binding.runtime_unit_epochs) {
    if (candidate.runtime_unit_id == control_path.provider_runtime_unit_id) {
      if (epoch != nullptr) {
        fail("runtime participant duplicates the provider runtime-unit epoch");
      }
      epoch = &candidate;
    }
  }
  if (output == nullptr || epoch == nullptr) {
    fail("runtime product output or current runtime epoch is absent");
  }
  return {&group->products.front(), output->content_digest,
          epoch->runtime_epoch};
}

[[nodiscard]] const core::RetainedProductDTO &
require_control_product(const RetainedBundle &retained_bundle,
                        const core::ControlDomainId &control_domain_id) {
  const core::RetainedProductDTO *result = nullptr;
  for (const auto &group :
       retained_bundle.value().compiled_products.control_domain_products) {
    if (group.control_domain_id != control_domain_id) {
      continue;
    }
    for (const auto &product : group.products) {
      if (product.product_kind ==
          core::RetainedProductKind::kControlDomainControl) {
        if (result != nullptr) {
          fail("retained bundle duplicates the control-domain product");
        }
        result = &product;
      }
    }
  }
  if (result == nullptr) {
    fail("retained bundle omits the control-domain product");
  }
  return *result;
}

[[nodiscard]] const core::RetainedArtifactRefDTO &
require_control_artifact(const core::RetainedProductDTO &product) {
  const core::RetainedArtifactRefDTO *result = nullptr;
  for (const auto &artifact : product.artifact_refs) {
    if (artifact.artifact_role_id.value() ==
        detail::kControlDomainControlArtifactRole) {
      if (result != nullptr) {
        fail("control-domain product duplicates its primary artifact");
      }
      result = &artifact;
    }
  }
  if (result == nullptr) {
    fail("control-domain product omits its primary artifact");
  }
  return *result;
}

[[nodiscard]] core::ResolvedStopRuntimeTargetDTO make_runtime_target(
    const core::RuntimeControlPathDTO &control_path,
    const RuntimeProduct &runtime_product,
    std::optional<core::EndpointRoleId> endpoint_role_id = std::nullopt,
    std::optional<std::string> logical_endpoint_symbol = std::nullopt) {
  return {
      control_path.provider_instance_id,
      control_path.provider_manifest_id,
      control_path.provider_manifest_revision,
      control_path.provider_runtime_unit_id,
      runtime_product.product->product_id,
      runtime_product.product->product_contract_id,
      runtime_product.product->product_contract_revision,
      runtime_product.content_digest,
      control_path.provider_runtime_epoch,
      runtime_product.runtime_epoch,
      std::move(endpoint_role_id),
      std::move(logical_endpoint_symbol),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
}

void require_endpoint(
    const core::ControllerLifecycleExpectedDomainDTO &expected,
    const core::RuntimeControlPathDTO &control_path,
    const core::EndpointRoleId &role, std::string_view symbol,
    bool command_endpoint) {
  const auto count = std::count_if(
      expected.expected_endpoints.begin(), expected.expected_endpoints.end(),
      [&](const auto &endpoint) {
        return endpoint.control_binding_id == control_path.control_binding_id &&
               endpoint.control_binding_revision ==
                   control_path.control_binding_revision &&
               endpoint.control_route_id == control_path.control_route_id &&
               endpoint.endpoint_role_id == role &&
               endpoint.logical_endpoint_symbol == symbol &&
               (command_endpoint ? endpoint.is_command_endpoint
                                 : endpoint.is_state_endpoint);
      });
  if (count != 1) {
    fail("Stop endpoint does not match the verified active route");
  }
}

void require_active_configuration(
    const core::ActiveControlConfigurationDTO &active,
    const core::ControllerLifecycleExpectedDomainDTO &expected,
    const core::RuntimeParticipantBindingDTO &binding,
    const core::RuntimeControlPathDTO &control_path) {
  if (!active.locally_valid() ||
      active.state != core::ActiveControlConfigurationStateDTO::kReconciled ||
      active.control_domain_id != expected.control_domain_id ||
      active.control_domain_revision != expected.control_domain_revision ||
      active.controller_topology_id != expected.controller_topology_id ||
      active.topology_revision != expected.topology_revision ||
      active.activation_set_id !=
          expected.desired_configuration.activation_set_id ||
      active.source_runtime_unit_id != expected.source_runtime_unit_id ||
      active.source_runtime_epoch != expected.source_runtime_epoch ||
      active.active_control_configuration_id !=
          control_path.active_control_configuration_id ||
      active.active_control_configuration_generation !=
          control_path.active_control_configuration_generation ||
      active.binding_routes.size() !=
          expected.desired_configuration.binding_routes.size()) {
    fail("active control configuration is not the reconciled expected closure");
  }
  for (std::size_t index = 0U; index < active.binding_routes.size(); ++index) {
    const auto &actual = active.binding_routes[index];
    const auto &desired = expected.desired_configuration.binding_routes[index];
    if (actual.control_binding_id != desired.control_binding_id ||
        actual.control_binding_revision != desired.control_binding_revision ||
        actual.control_route_id != desired.control_route_id) {
      fail("active binding-route table differs from the verified desired "
           "closure");
    }
  }
  if (std::count_if(active.binding_routes.begin(), active.binding_routes.end(),
                    [&](const auto &route) {
                      return route.control_binding_id ==
                                 control_path.control_binding_id &&
                             route.control_binding_revision ==
                                 control_path.control_binding_revision &&
                             route.control_route_id ==
                                 control_path.control_route_id;
                    }) != 1) {
    fail("runtime control path is not in the complete active route table");
  }

  for (const auto &resource : binding.resources) {
    const auto projection =
        std::find_if(active.kinematic_resource_source_projections.begin(),
                     active.kinematic_resource_source_projections.end(),
                     [&](const auto &candidate) {
                       return candidate.resource_id == resource.resource_id;
                     });
    if (projection == active.kinematic_resource_source_projections.end() ||
        projection->provider_instance_id != control_path.provider_instance_id ||
        projection->provider_manifest_id != control_path.provider_manifest_id ||
        projection->provider_manifest_revision !=
            control_path.provider_manifest_revision ||
        projection->state_authority_id != control_path.state_authority_id ||
        projection->state_authority_class !=
            control_path.state_authority_class ||
        std::count_if(projection->contributing_control_bindings.begin(),
                      projection->contributing_control_bindings.end(),
                      [&](const auto &source) {
                        return source.control_binding_id ==
                                   control_path.control_binding_id &&
                               source.control_binding_revision ==
                                   control_path.control_binding_revision;
                      }) != 1) {
      fail("active state-authority projection does not cover the participant");
    }
  }
}

[[nodiscard]] core::ResolvedStopContainmentScopeDTO
parse_scope(const CanonicalValue &value, std::string_view path,
            const core::RuntimeParticipantBindingDTO &binding,
            const core::ControllerLifecycleExpectedDomainDTO &expected,
            const core::RuntimeControlPathDTO &control_path) {
  const core::ActivationSetId activation_set_id(
      text(member(value, "activation_set_id", path), path));
  if (activation_set_id != control_path.activation_set_id) {
    fail("Stop containment scope selects a non-active activation set");
  }
  const auto kind = text(member(value, "scope_kind", path), path);
  if (kind == "resource") {
    std::vector<core::ResourceId> resources;
    for (const auto &item : array(member(value, "resource_ids", path), path)) {
      resources.emplace_back(text(item, path));
    }
    std::vector<core::ResourceId> participant_resources;
    for (const auto &row : binding.resources) {
      participant_resources.push_back(row.resource_id);
    }
    std::sort(participant_resources.begin(), participant_resources.end());
    if (resources != participant_resources) {
      fail("resource Stop scope differs from the participant resource closure");
    }
    return core::ResolvedStopResourceContainmentScopeDTO{activation_set_id,
                                                         std::move(resources)};
  }
  if (kind == "controller_set") {
    const auto &role_values =
        array(member(value, "active_controller_role_ids", path), path);
    const auto &symbol_values =
        array(member(value, "logical_controller_symbols", path), path);
    if (role_values.size() != symbol_values.size()) {
      fail("controller Stop scope has mismatched role/symbol cardinality");
    }
    std::vector<core::ResolvedStopControllerContainmentRowDTO> rows;
    for (std::size_t index = 0U; index < role_values.size(); ++index) {
      core::ResolvedStopControllerContainmentRowDTO row{
          core::ControllerRoleId(text(role_values[index], path)),
          text(symbol_values[index], path)};
      const auto controller = std::find_if(
          expected.expected_controllers.begin(),
          expected.expected_controllers.end(), [&](const auto &candidate) {
            return candidate.controller_role_id == row.controller_role_id;
          });
      const auto topology_controller = std::find_if(
          expected.controller_topology.controllers.begin(),
          expected.controller_topology.controllers.end(),
          [&](const auto &candidate) {
            return candidate.controller_role_id == row.controller_role_id;
          });
      if (controller == expected.expected_controllers.end() ||
          topology_controller ==
              expected.controller_topology.controllers.end()) {
        fail("controller Stop scope differs from the verified topology");
      }
      rows.push_back(std::move(row));
    }
    return core::ResolvedStopControllerSetContainmentScopeDTO{activation_set_id,
                                                              std::move(rows)};
  }
  if (kind == "control_domain") {
    core::ResolvedStopControlDomainContainmentScopeDTO result{
        activation_set_id,
        core::ControlDomainId(
            text(member(value, "control_domain_id", path), path)),
        core::ControlDomainRevision(
            text(member(value, "control_domain_revision", path), path))};
    if (result.control_domain_id != control_path.control_domain_id ||
        result.control_domain_revision !=
            control_path.control_domain_revision) {
      fail("domain Stop scope differs from the active control domain");
    }
    return result;
  }
  fail("Stop containment scope kind is unknown");
}

[[nodiscard]] core::CommissionedStopInterfaceDTO
parse_commissioned(const CanonicalValue &value, std::string_view path) {
  const auto &kind = member(value, "external_resource_kind", path);
  return {
      core::CommissionedStopInterfaceId(
          text(member(value, "commissioned_stop_interface_id", path), path)),
      core::CommissionedStopInterfaceRevision(text(
          member(value, "commissioned_stop_interface_revision", path), path)),
      core::ControlDomainId(
          text(member(value, "control_domain_id", path), path)),
      core::ControlDomainRevision(
          text(member(value, "control_domain_revision", path), path)),
      core::ControlBindingId(
          text(member(value, "control_binding_id", path), path)),
      core::ControlBindingRevision(
          text(member(value, "control_binding_revision", path), path)),
      core::ControlRouteId(text(member(value, "control_route_id", path), path)),
      core::ProviderInstanceId(
          text(member(value, "provider_instance_id", path), path)),
      core::ProviderManifestId(
          text(member(value, "provider_manifest_id", path), path)),
      core::ProviderManifestRevision(
          text(member(value, "provider_manifest_revision", path), path)),
      core::RegisteredStopCommandKindId(
          text(member(value, "registered_command_kind_id", path), path)),
      parse_contract(member(value, "provider_contract_ref", path), path),
      core::EndpointRoleId(text(member(value, "endpoint_role_id", path), path)),
      core::StopActionClass::kSafetySystemRequest,
      parse_safety_action(text(
          member(value, "safety_system_request_action_class", path), path)),
      optional_strong<core::ExternalResourceAlias>(
          member(value, "external_resource_alias", path), path),
      is_null(kind) ? std::nullopt
                    : std::optional<core::ExternalResourceKind>(
                          parse_external_kind(text(kind, path))),
      optional_strong<core::SiteOverlayValueRef>(
          member(value, "site_overlay_value_ref", path), path),
      parse_artifact(member(value, "commissioning_evidence_artifact_ref", path),
                     path),
  };
}

} // namespace

core::ResolvedStopBasePlanDTO resolve_verified_stop_base_plan(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    const core::RuntimeParticipantBinding &participant_binding) {
  if (!verify_retained_bundle_identity(retained_bundle)) {
    fail("retained bundle identity is invalid");
  }
  const auto &binding = participant_binding.value();
  if (!binding.control_path.has_value() ||
      binding.retained_bundle_id != retained_bundle.retained_bundle_id() ||
      binding.graph_revision !=
          retained_bundle.value().cell_graph.graph_revision) {
    fail("runtime participant does not belong to the retained bundle");
  }
  const auto &control_path = *binding.control_path;
  const auto &expected = expected_domain.value();
  if (expected.control_domain_id != control_path.control_domain_id ||
      expected.control_domain_revision !=
          control_path.control_domain_revision ||
      expected.controller_topology_id != control_path.controller_topology_id ||
      expected.topology_revision != control_path.topology_revision ||
      expected.desired_configuration.activation_set_id !=
          control_path.activation_set_id) {
    fail("runtime control path differs from the verified expected domain");
  }
  require_active_configuration(active_configuration, expected, binding,
                               control_path);

  const auto &product =
      require_control_product(retained_bundle, control_path.control_domain_id);
  const auto &artifact = require_control_artifact(product);
  if (product.product_id != expected.product_id ||
      product.product_contract_id != expected.product_contract_id ||
      product.product_contract_revision != expected.product_contract_revision ||
      artifact.content_digest != expected.control_artifact_digest) {
    fail("verified expected-domain product differs from the retained product");
  }
  const RetainedProductArtifactQuery query{
      retained_bundle.retained_bundle_id(), product.product_id,
      product.product_kind, artifact.artifact_role_id};
  const auto loaded = artifact_source.get_verified(query);
  if (!loaded.has_value() || loaded->query() != query ||
      loaded->product_contract_id() != product.product_contract_id ||
      loaded->product_contract_revision() !=
          product.product_contract_revision ||
      loaded->artifact_ref() != artifact) {
    fail("exact retained control-domain artifact is absent or mismatched");
  }
  detail::require_control_domain_control_matches_retained_bundle(
      loaded->bytes(), retained_bundle.value(), control_path.control_domain_id);
  const auto root =
      detail::parse_and_verify_control_domain_control(loaded->bytes());
  const auto runtime_product =
      require_runtime_product(retained_bundle, binding, control_path);

  const CanonicalValue *selected_stop = nullptr;
  for (const auto &candidate :
       array(member(root, "stop_applicability", "control"), "control")) {
    if (text(member(candidate, "control_binding_id", "stop"), "stop") ==
            control_path.control_binding_id.value() &&
        text(member(candidate, "control_binding_revision", "stop"), "stop") ==
            control_path.control_binding_revision.value() &&
        text(member(candidate, "control_route_id", "stop"), "stop") ==
            control_path.control_route_id.value()) {
      if (selected_stop != nullptr) {
        fail("control artifact duplicates the selected Stop row");
      }
      selected_stop = &candidate;
    }
  }
  if (selected_stop == nullptr ||
      text(member(*selected_stop, "disposition", "stop"), "stop") !=
          "stoppable") {
    fail("active route has no stoppable retained plan");
  }

  std::vector<core::CommissionedStopInterfaceDTO> commissioned;
  for (const auto &row :
       array(member(root, "commissioned_stop_interfaces", "control"),
             "control")) {
    commissioned.push_back(parse_commissioned(row, "commissioned"));
  }

  core::ResolvedStopBasePlanDTO plan{
      core::ResolvedStopBasePlanId("pending"),
      retained_bundle.value().cell_graph.cell_id,
      retained_bundle.retained_bundle_id(),
      retained_bundle.value().cell_graph.graph_revision,
      core::ResolvedStopTemplateRefDTO{
          product.product_id, product.product_contract_id,
          product.product_contract_revision, artifact,
          text(member(*selected_stop, "logical_stop_symbol", "stop"), "stop")},
      control_path.control_domain_id,
      control_path.control_domain_revision,
      control_path.control_binding_id,
      control_path.control_binding_revision,
      control_path.controller_topology_id,
      control_path.topology_revision,
      control_path.control_route_id,
      control_path.activation_set_id,
      active_configuration.active_control_configuration_id,
      active_configuration.active_control_configuration_generation,
      active_configuration.source_runtime_unit_id,
      active_configuration.source_runtime_epoch,
      active_configuration.source_sequence,
      core::StopEvidencePolicyId(text(
          member(*selected_stop, "stop_evidence_policy_id", "stop"), "stop")),
      core::StopEvidencePolicyRevision(
          text(member(*selected_stop, "stop_evidence_policy_revision", "stop"),
               "stop")),
      parse_granularity(text(
          member(*selected_stop, "containment_granularity", "stop"), "stop")),
      parse_budget(member(*selected_stop, "deadline_budget", "stop"),
                   "stop.deadline_budget"),
      {},
  };
  if (plan.stop_evidence_policy_id != control_path.stop_evidence_policy_id ||
      plan.stop_evidence_policy_revision !=
          control_path.stop_evidence_policy_revision ||
      plan.containment_granularity != control_path.containment_granularity) {
    fail("retained Stop policy differs from the runtime control path");
  }

  const auto &stages =
      array(member(*selected_stop, "escalation_stages", "stop"), "stop");
  plan.escalation_stages.reserve(stages.size());
  for (std::size_t index = 0U; index < stages.size(); ++index) {
    const auto &source = stages[index];
    const auto stage_id = core::StopEscalationStageId(
        text(member(source, "stop_escalation_stage_id", "stage"), "stage"));
    auto command = parse_command(member(source, "containment_command", "stage"),
                                 "stage.command");
    std::optional<core::EndpointRoleId> command_endpoint_role;
    std::optional<std::string> command_endpoint_symbol;
    const auto &command_endpoint =
        member(source, "provider_command_endpoint", "stage");
    if (!is_null(command_endpoint)) {
      const auto endpoint_contract = parse_contract(
          member(command_endpoint, "provider_contract_ref", "command endpoint"),
          "command endpoint");
      command_endpoint_role = core::EndpointRoleId(
          text(member(command_endpoint, "endpoint_role_id", "command endpoint"),
               "command endpoint"));
      command_endpoint_symbol =
          text(member(command_endpoint, "logical_endpoint_symbol",
                      "command endpoint"),
               "command endpoint");
      if (!command.provider_contract_ref.has_value() ||
          endpoint_contract != *command.provider_contract_ref) {
        fail("provider command endpoint contract differs from the stage");
      }
      require_endpoint(expected, control_path, *command_endpoint_role,
                       *command_endpoint_symbol, true);
    }
    auto command_target =
        make_runtime_target(control_path, runtime_product,
                            command_endpoint_role, command_endpoint_symbol);

    core::ResolvedStopCommandQualificationDTO qualification;
    const auto stage_kind =
        parse_stage_kind(text(member(source, "stage_kind", "stage"), "stage"));
    qualification.stop_action_class =
        stage_kind == core::StopEscalationStageKind::kSoftwareHard
            ? core::StopActionClass::kSoftwareHard
        : stage_kind == core::StopEscalationStageKind::kSafetySystemRequest
            ? core::StopActionClass::kSafetySystemRequest
            : core::StopActionClass::kControlled;

    const auto &default_parameters =
        member(source, "default_contract_parameters", "stage");
    std::optional<core::JsonObjectDTO> parameters;
    if (!is_null(default_parameters)) {
      parameters = parse_json_object(default_parameters, "stage parameters");
      if (stage_kind == core::StopEscalationStageKind::kNominal &&
          command.command_kind ==
              core::StopContainmentCommandKind::kMotionControlledStop) {
        const auto substitution_parameter =
            parameters->find("recoverable_substitution_mode");
        const bool requests_substitution =
            substitution_parameter != parameters->end() &&
            !std::holds_alternative<std::nullptr_t>(
                substitution_parameter->second.value);
        if (requests_substitution) {
          if (!command.provider_contract_ref.has_value()) {
            fail("nominal substitution has no provider contract");
          }
          qualification.controlled_substitution_contract =
              core::ControlledStopSubstitutionContractDTO{
                  *command.provider_contract_ref,
                  core::MotionControlledStopMode::kSoftOnPath,
                  core::MotionControlledStopMode::kSpringBackHold};
        }
      }
    }

    if (stage_kind == core::StopEscalationStageKind::kSafetySystemRequest) {
      const auto &interface =
          member(source, "commissioned_stop_interface", "stage");
      if (is_null(interface)) {
        fail("safety-system stage omits its commissioned interface");
      }
      const auto interface_id = core::CommissionedStopInterfaceId(
          text(member(interface, "commissioned_stop_interface_id", "stage"),
               "stage"));
      const auto found = std::find_if(
          commissioned.begin(), commissioned.end(), [&](const auto &candidate) {
            return candidate.commissioned_stop_interface_id == interface_id;
          });
      if (found == commissioned.end() ||
          std::find_if(
              std::next(found), commissioned.end(), [&](const auto &candidate) {
                return candidate.commissioned_stop_interface_id == interface_id;
              }) != commissioned.end()) {
        fail("safety-system stage has no sole commissioned interface");
      }
      qualification.safety_system_request_action_class =
          found->safety_system_request_action_class;
      qualification.commissioned_stop_interface_id =
          found->commissioned_stop_interface_id;
      qualification.commissioned_stop_interface_revision =
          found->commissioned_stop_interface_revision;
      qualification.commissioning_evidence_artifact_ref =
          found->commissioning_evidence_artifact_ref;
      if (found->provider_instance_id != control_path.provider_instance_id ||
          found->provider_manifest_id != control_path.provider_manifest_id ||
          found->provider_manifest_revision !=
              control_path.provider_manifest_revision ||
          found->control_domain_id != control_path.control_domain_id ||
          found->control_domain_revision !=
              control_path.control_domain_revision ||
          found->control_binding_id != control_path.control_binding_id ||
          found->control_binding_revision !=
              control_path.control_binding_revision ||
          found->control_route_id != control_path.control_route_id ||
          found->endpoint_role_id != command_target.endpoint_role_id) {
        fail("commissioned Stop interface lacks a matching current runtime "
             "occurrence");
      }
      if (found->external_resource_alias.has_value()) {
        const auto external =
            std::find_if(retained_bundle.value().external_resources.begin(),
                         retained_bundle.value().external_resources.end(),
                         [&](const auto &candidate) {
                           return candidate.external_resource_alias ==
                                  *found->external_resource_alias;
                         });
        if (external == retained_bundle.value().external_resources.end() ||
            !retained_bundle.value().selected_site_overlay_ref.has_value() ||
            external->resource_kind != found->external_resource_kind ||
            external->provider_instance_id != found->provider_instance_id ||
            external->provider_manifest_id != found->provider_manifest_id ||
            external->provider_manifest_revision !=
                found->provider_manifest_revision ||
            external->endpoint_role_id != found->endpoint_role_id ||
            external->site_overlay_value_ref != found->site_overlay_value_ref) {
          fail("commissioned external Stop interface has no retained overlay");
        }
        command_target.external_resource_alias =
            external->external_resource_alias;
        command_target.external_resource_kind = external->resource_kind;
        command_target.site_overlay_id =
            retained_bundle.value().selected_site_overlay_ref->site_overlay_id;
        command_target.site_overlay_revision =
            retained_bundle.value()
                .selected_site_overlay_ref->site_overlay_revision;
        command_target.site_overlay_non_secret_digest =
            retained_bundle.value()
                .selected_site_overlay_ref->non_secret_digest;
        command_target.site_overlay_value_ref =
            external->site_overlay_value_ref;
      }
    }

    const auto &scopes =
        array(member(source, "containment_scopes", "stage"), "stage");
    if (scopes.size() != 1U) {
      fail("selected Stop stage has no sole active containment scope");
    }
    auto scope = parse_scope(scopes.front(), "stage.scope", binding, expected,
                             control_path);

    std::map<std::string, const CanonicalValue *> binding_sources;
    for (const auto &row :
         array(member(source, "binding_state_authority_sources", "stage"),
               "stage")) {
      const auto key = text(member(row, "evidence_item_id", "stage"), "stage");
      if (!binding_sources.emplace(key, &row).second) {
        fail("stage duplicates a binding-state evidence source");
      }
    }
    std::map<std::string, const CanonicalValue *> provider_sources;
    for (const auto &row : array(
             member(source, "provider_evidence_endpoints", "stage"), "stage")) {
      const auto key = text(member(row, "evidence_item_id", "stage"), "stage");
      if (!provider_sources.emplace(key, &row).second) {
        fail("stage duplicates a provider evidence source");
      }
    }

    std::vector<core::ResolvedStopEvidenceRequirementDTO> evidence;
    for (const auto &row :
         array(member(source, "all_of_evidence_requirements", "stage"),
               "stage")) {
      auto requirement = parse_evidence(row, "stage.evidence");
      auto resolved_target = make_runtime_target(control_path, runtime_product);
      std::optional<core::StateAuthorityId> state_authority_id;
      std::optional<core::StateAuthorityClass> state_authority_class;
      std::optional<core::ProviderContractRefDTO> provider_contract_ref;
      if (requirement.source_requirement.source_kind ==
          core::StopEvidenceSourceKind::kBindingStateAuthority) {
        const auto found =
            binding_sources.find(requirement.evidence_item_id.value());
        if (found == binding_sources.end()) {
          fail("binding-state evidence has no exact retained source");
        }
        const auto &source_row = *found->second;
        const auto endpoint = core::EndpointRoleId(text(
            member(source_row, "state_endpoint_role_id", "evidence source"),
            "evidence source"));
        const auto symbol = text(
            member(source_row, "logical_endpoint_symbol", "evidence source"),
            "evidence source");
        require_endpoint(expected, control_path, endpoint, symbol, false);
        resolved_target.endpoint_role_id = endpoint;
        resolved_target.logical_endpoint_symbol = symbol;
        state_authority_id = core::StateAuthorityId(
            text(member(source_row, "state_authority_id", "evidence source"),
                 "evidence source"));
        state_authority_class = parse_authority_class(
            text(member(source_row, "state_authority_class", "evidence source"),
                 "evidence source"));
        if (*state_authority_id != control_path.state_authority_id ||
            *state_authority_class != control_path.state_authority_class) {
          fail("binding-state evidence differs from the active authority");
        }
      } else if (requirement.source_requirement.source_kind ==
                 core::StopEvidenceSourceKind::kProviderEvidence) {
        const auto found =
            provider_sources.find(requirement.evidence_item_id.value());
        if (found == provider_sources.end()) {
          fail("provider evidence has no exact retained endpoint");
        }
        const auto &source_row = *found->second;
        const auto endpoint = core::EndpointRoleId(
            text(member(source_row, "endpoint_role_id", "evidence source"),
                 "evidence source"));
        const auto symbol = text(
            member(source_row, "logical_endpoint_symbol", "evidence source"),
            "evidence source");
        require_endpoint(expected, control_path, endpoint, symbol, false);
        resolved_target.endpoint_role_id = endpoint;
        resolved_target.logical_endpoint_symbol = symbol;
        provider_contract_ref = parse_contract(
            member(source_row, "provider_contract_ref", "evidence source"),
            "evidence source");
      }
      const auto source_kind = requirement.source_requirement.source_kind;
      evidence.push_back({
          std::move(requirement),
          core::ResolvedStopEvidenceSourceDTO{
              source_kind, std::move(resolved_target),
              std::move(state_authority_id), std::move(state_authority_class),
              std::move(provider_contract_ref)},
      });
    }

    plan.escalation_stages.push_back({
        static_cast<std::uint64_t>(index),
        stage_id,
        stage_kind,
        parse_granularity(
            text(member(source, "containment_granularity", "stage"), "stage")),
        std::move(command),
        std::move(parameters),
        std::move(qualification),
        std::move(command_target),
        std::move(scope),
        core::StopEvidenceConjunction::kAllOf,
        std::move(evidence),
    });
  }

  auto made = core::MakeResolvedStopBasePlan(std::move(plan));
  if (!made.ok()) {
    fail("resolved plan validation failed: " + made.error().message);
  }
  return std::move(made.value());
}

core::Result<core::RuntimeParticipantBinding>
rebind_runtime_participant_to_active_configuration(
    const RetainedBundle &retained_bundle,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    const core::RuntimeParticipantBinding &participant_binding) {
  try {
    if (!verify_retained_bundle_identity(retained_bundle) ||
        !active_configuration.locally_valid() ||
        active_configuration.state !=
            core::ActiveControlConfigurationStateDTO::kReconciled) {
      fail("route readiness requires one verified bundle and reconciled "
           "active configuration");
    }

    const auto &expected = expected_domain.value();
    if (!expected.locally_valid() ||
        active_configuration.control_domain_id != expected.control_domain_id ||
        active_configuration.control_domain_revision !=
            expected.control_domain_revision ||
        active_configuration.controller_topology_id !=
            expected.controller_topology_id ||
        active_configuration.topology_revision != expected.topology_revision ||
        active_configuration.activation_set_id !=
            expected.desired_configuration.activation_set_id ||
        active_configuration.source_runtime_unit_id !=
            expected.source_runtime_unit_id ||
        active_configuration.source_runtime_epoch !=
            expected.source_runtime_epoch ||
        active_configuration.binding_routes.size() !=
            expected.desired_configuration.binding_routes.size()) {
      fail("active configuration differs from the verified expected domain");
    }
    for (std::size_t index = 0U;
         index < active_configuration.binding_routes.size(); ++index) {
      const auto &active = active_configuration.binding_routes[index];
      const auto &desired =
          expected.desired_configuration.binding_routes[index];
      if (active.control_binding_id != desired.control_binding_id ||
          active.control_binding_revision != desired.control_binding_revision ||
          active.control_route_id != desired.control_route_id) {
        fail("active configuration is not the complete desired route table");
      }
    }

    auto rebound = participant_binding.value();
    if (rebound.retained_bundle_id != retained_bundle.retained_bundle_id() ||
        rebound.graph_revision !=
            retained_bundle.value().cell_graph.graph_revision ||
        !rebound.control_path.has_value()) {
      fail("runtime participant is outside the retained control domain");
    }
    auto &path = *rebound.control_path;
    const auto &graph = retained_bundle.value().cell_graph;
    const auto graph_binding = std::find_if(
        graph.control_bindings.begin(), graph.control_bindings.end(),
        [&](const auto &candidate) {
          return candidate.control_binding_id == path.control_binding_id;
        });
    if (graph_binding == graph.control_bindings.end() ||
        std::count_if(
            graph.control_bindings.begin(), graph.control_bindings.end(),
            [&](const auto &candidate) {
              return candidate.control_binding_id == path.control_binding_id;
            }) != 1 ||
        graph_binding->control_binding_revision !=
            path.control_binding_revision ||
        graph_binding->controllable_component_id !=
            rebound.controllable_component_id ||
        graph_binding->command_target_id != rebound.command_target_id ||
        graph_binding->capability_id != rebound.capability_id ||
        graph_binding->capability_definition_revision !=
            rebound.capability_definition_revision ||
        graph_binding->control_domain_id != path.control_domain_id ||
        graph_binding->control_interface_contract_id !=
            path.control_interface_contract_id ||
        graph_binding->control_interface_revision !=
            path.control_interface_revision ||
        graph_binding->controller_topology_id != path.controller_topology_id ||
        graph_binding->topology_revision != path.topology_revision ||
        graph_binding->provider_instance_id != path.provider_instance_id ||
        graph_binding->provider_manifest_id != path.provider_manifest_id ||
        graph_binding->provider_manifest_revision !=
            path.provider_manifest_revision ||
        graph_binding->provider_runtime_unit_id !=
            path.provider_runtime_unit_id ||
        graph_binding->state_authority_id != path.state_authority_id ||
        graph_binding->state_authority_class != path.state_authority_class ||
        graph_binding->stop_evidence_policy_id !=
            path.stop_evidence_policy_id ||
        graph_binding->stop_evidence_policy_revision !=
            path.stop_evidence_policy_revision ||
        graph_binding->containment_granularity !=
            path.containment_granularity) {
      fail("runtime participant differs from its retained control binding");
    }
    if (path.control_domain_id != expected.control_domain_id ||
        path.control_domain_revision != expected.control_domain_revision ||
        path.controller_topology_id != expected.controller_topology_id ||
        path.topology_revision != expected.topology_revision) {
      fail("runtime participant differs from the verified control domain");
    }

    const auto selected = std::find_if(
        active_configuration.binding_routes.begin(),
        active_configuration.binding_routes.end(), [&](const auto &candidate) {
          return candidate.control_binding_id == path.control_binding_id;
        });
    if (selected == active_configuration.binding_routes.end() ||
        selected->control_binding_revision != path.control_binding_revision ||
        std::find(graph_binding->legal_control_route_ids.begin(),
                  graph_binding->legal_control_route_ids.end(),
                  selected->control_route_id) ==
            graph_binding->legal_control_route_ids.end()) {
      fail("runtime participant has no exact legal selected route");
    }

    path.control_route_id = selected->control_route_id;
    path.activation_set_id = active_configuration.activation_set_id;
    path.active_control_configuration_id =
        active_configuration.active_control_configuration_id;
    path.active_control_configuration_generation =
        active_configuration.active_control_configuration_generation;
    path.runtime_scale_qualification.reset();
    const auto scale = std::find_if(
        graph_binding->runtime_scale_route_qualifications.begin(),
        graph_binding->runtime_scale_route_qualifications.end(),
        [&](const auto &candidate) {
          return candidate.control_route_id == path.control_route_id;
        });
    if (scale != graph_binding->runtime_scale_route_qualifications.end()) {
      if (std::find_if(
              std::next(scale),
              graph_binding->runtime_scale_route_qualifications.end(),
              [&](const auto &candidate) {
                return candidate.control_route_id == path.control_route_id;
              }) != graph_binding->runtime_scale_route_qualifications.end()) {
        fail("retained control binding duplicates route scaling qualification");
      }
      path.runtime_scale_qualification = *scale;
    }

    const auto source_epoch = std::find_if(
        rebound.runtime_unit_epochs.begin(), rebound.runtime_unit_epochs.end(),
        [&](const auto &candidate) {
          return candidate.runtime_unit_id ==
                 active_configuration.source_runtime_unit_id;
        });
    if (source_epoch == rebound.runtime_unit_epochs.end() ||
        std::find_if(std::next(source_epoch), rebound.runtime_unit_epochs.end(),
                     [&](const auto &candidate) {
                       return candidate.runtime_unit_id ==
                              active_configuration.source_runtime_unit_id;
                     }) != rebound.runtime_unit_epochs.end()) {
      fail("runtime participant omits the authoritative domain runtime unit");
    }
    source_epoch->runtime_epoch = active_configuration.source_runtime_epoch;

    auto sealed = core::verify_runtime_participant_binding(std::move(rebound));
    if (!sealed.ok()) {
      return readiness_error<core::RuntimeParticipantBinding>(
          "route_readiness_rebound_binding_invalid", sealed.error().message);
    }
    require_active_configuration(active_configuration, expected,
                                 sealed.value().value(),
                                 *sealed.value().value().control_path);
    return sealed;
  } catch (const std::exception &exception) {
    return readiness_error<core::RuntimeParticipantBinding>(
        "route_readiness_rebind_failed", exception.what());
  }
}

core::Result<core::RuntimeParticipantStopReadiness>
store_and_verify_runtime_participant_stop_readiness(
    const RetainedBundle &retained_bundle, core::ResolvedStopBasePlanDTO plan,
    core::ComponentLifecycleGeneration component_lifecycle_generation,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::RuntimeParticipantBinding &participant_binding,
    core::IResolvedStopBasePlanRepository &repository) {
  if (!verify_retained_bundle_identity(retained_bundle) ||
      plan.retained_bundle_id != retained_bundle.retained_bundle_id())
    return readiness_error<core::RuntimeParticipantStopReadiness>(
        "stop_readiness_retained_timing_mismatch",
        "Stop readiness requires the exact retained bundle");
  std::optional<std::int64_t> maximum_duration;
  for (const auto &selected : retained_bundle.value().stop_timing_parameters) {
    if (selected.control_binding_id != plan.control_binding_id)
      continue;
    if (selected.control_binding_revision != plan.control_binding_revision)
      return readiness_error<core::RuntimeParticipantStopReadiness>(
          "stop_readiness_retained_timing_mismatch",
          "Stop timing differs from the exact binding revision");
    maximum_duration = selected.timing.operational_timeout_ns -
                       selected.timing.command_latency_allowance_ns -
                       selected.timing.settling_allowance_ns;
  }
  const auto verified = core::VerifyResolvedStopBasePlan(plan);
  if (!verified.ok()) {
    return core::Result<core::RuntimeParticipantStopReadiness>::Err(
        verified.error());
  }
  const auto expected_id = plan.resolved_stop_base_plan_id;
  const auto stored = repository.PutIfAbsent(plan);
  if (!stored.ok()) {
    return core::Result<core::RuntimeParticipantStopReadiness>::Err(
        stored.error());
  }
  if (stored.value() != expected_id) {
    return core::Result<core::RuntimeParticipantStopReadiness>::Err(
        core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
            "resolved Stop base-plan repository returned a different identity",
            {{"reason",
              "resolved_stop_base_plan_repository_identity_mismatch"}}});
  }
  const auto loaded = repository.LoadVerified(expected_id);
  if (!loaded.ok()) {
    return core::Result<core::RuntimeParticipantStopReadiness>::Err(
        loaded.error());
  }
  if (loaded.value() != plan) {
    return core::Result<core::RuntimeParticipantStopReadiness>::Err(
        core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
            "resolved Stop base-plan repository readback disagrees",
            {{"reason",
              "resolved_stop_base_plan_repository_readback_mismatch"}}});
  }
  return core::verify_runtime_participant_stop_readiness(
      core::RuntimeParticipantStopReadinessDTO{
          participant_binding.value().controllable_component_id,
          component_lifecycle_generation, expected_domain.value(),
          std::move(loaded.value()), maximum_duration},
      participant_binding, expected_domain);
}

core::Result<std::vector<VerifiedRouteReadinessParticipant>>
resolve_store_and_verify_route_readiness_batch(
    const RetainedBundle &retained_bundle,
    const IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ControllerLifecycleExpectedDomain &expected_domain,
    const core::ActiveControlConfigurationDTO &active_configuration,
    const std::span<const core::RouteSelectionComponentExpectationDTO>
        component_expectations,
    const std::span<const core::RuntimeParticipantBinding> participant_bindings,
    core::IResolvedStopBasePlanRepository &repository) {
  struct Pending final {
    core::RuntimeParticipantBinding binding;
    core::ComponentLifecycleGeneration component_generation;
    core::ResolvedStopBasePlanDTO plan;
  };

  try {
    const auto &desired = expected_domain.value().desired_configuration;
    if (!verify_retained_bundle_identity(retained_bundle) ||
        !active_configuration.locally_valid() ||
        active_configuration.state !=
            core::ActiveControlConfigurationStateDTO::kReconciled ||
        desired.binding_routes.empty() ||
        desired.binding_routes.size() >
            core::kMaxRouteReadinessPublicationRows ||
        participant_bindings.size() != desired.binding_routes.size() ||
        component_expectations.empty() ||
        component_expectations.size() > core::kMaxRouteSelectionComponents ||
        !std::is_sorted(
            component_expectations.begin(), component_expectations.end(),
            core::CanonicalRouteSelectionComponentExpectationLess) ||
        !std::all_of(component_expectations.begin(),
                     component_expectations.end(),
                     [](const auto &row) { return row.locally_valid(); })) {
      return readiness_error<std::vector<VerifiedRouteReadinessParticipant>>(
          "route_readiness_batch_shape_invalid",
          "route readiness batch is incomplete, noncanonical, or unreconciled");
    }
    for (std::size_t index = 1U; index < component_expectations.size();
         ++index) {
      if (component_expectations[index - 1U].controllable_component_id ==
          component_expectations[index].controllable_component_id) {
        return readiness_error<std::vector<VerifiedRouteReadinessParticipant>>(
            "route_readiness_component_duplicate",
            "route readiness duplicates a component generation");
      }
    }

    std::vector<Pending> pending;
    pending.reserve(desired.binding_routes.size());
    std::optional<std::pair<std::int64_t, std::int64_t>> shared_envelope;
    for (const auto &selected : desired.binding_routes) {
      const core::RuntimeParticipantBinding *representative = nullptr;
      for (const auto &candidate : participant_bindings) {
        if (!candidate.value().control_path.has_value()) {
          continue;
        }
        const auto &path = *candidate.value().control_path;
        if (path.control_binding_id == selected.control_binding_id &&
            path.control_binding_revision ==
                selected.control_binding_revision) {
          if (representative != nullptr) {
            return readiness_error<
                std::vector<VerifiedRouteReadinessParticipant>>(
                "route_readiness_binding_duplicate",
                "route readiness has multiple representatives for one binding");
          }
          representative = &candidate;
        }
      }
      if (representative == nullptr) {
        return readiness_error<std::vector<VerifiedRouteReadinessParticipant>>(
            "route_readiness_binding_missing",
            "route readiness omits a selected control binding");
      }

      const auto component = std::find_if(
          component_expectations.begin(), component_expectations.end(),
          [&](const auto &candidate) {
            return candidate.controllable_component_id ==
                   representative->value().controllable_component_id;
          });
      if (component == component_expectations.end()) {
        return readiness_error<std::vector<VerifiedRouteReadinessParticipant>>(
            "route_readiness_component_missing",
            "route readiness omits the selected binding component generation");
      }

      auto rebound = rebind_runtime_participant_to_active_configuration(
          retained_bundle, expected_domain, active_configuration,
          *representative);
      if (!rebound.ok()) {
        return core::Result<std::vector<VerifiedRouteReadinessParticipant>>::
            Err(rebound.error());
      }
      auto plan = resolve_verified_stop_base_plan(
          retained_bundle, artifact_source, expected_domain,
          active_configuration, rebound.value());
      const auto envelope =
          std::pair(plan.deadline_budget.outer_duration_ns,
                    plan.deadline_budget.kernel_completion_reserve_ns);
      if (shared_envelope.has_value() && *shared_envelope != envelope) {
        return readiness_error<std::vector<VerifiedRouteReadinessParticipant>>(
            "route_readiness_stop_envelope_mismatch",
            "selected participant Stop plans disagree on the cell envelope");
      }
      shared_envelope = envelope;
      pending.push_back(Pending{
          std::move(rebound.value()),
          component->expected_component_lifecycle_generation, std::move(plan)});
    }

    std::vector<VerifiedRouteReadinessParticipant> result;
    result.reserve(pending.size());
    for (auto &candidate : pending) {
      const auto plan_id = candidate.plan.resolved_stop_base_plan_id;
      auto readiness = store_and_verify_runtime_participant_stop_readiness(
          retained_bundle, std::move(candidate.plan),
          candidate.component_generation, expected_domain, candidate.binding,
          repository);
      if (!readiness.ok()) {
        return core::Result<std::vector<VerifiedRouteReadinessParticipant>>::
            Err(readiness.error());
      }
      const auto &path = *candidate.binding.value().control_path;
      core::RouteReadinessPublicationRowDTO row{
          candidate.binding.value().controllable_component_id,
          candidate.component_generation, path.control_binding_id,
          path.control_binding_revision, plan_id};
      result.push_back(VerifiedRouteReadinessParticipant{
          std::move(candidate.binding), std::move(readiness.value()),
          std::move(row)});
    }
    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) {
                return core::CanonicalRouteReadinessPublicationRowLess(
                    left.readiness_row, right.readiness_row);
              });
    if (!std::all_of(result.begin(), result.end(), [](const auto &row) {
          return row.readiness_row.locally_valid();
        })) {
      return readiness_error<std::vector<VerifiedRouteReadinessParticipant>>(
          "route_readiness_row_invalid",
          "resolved route readiness contains an invalid publication row");
    }
    return core::Result<std::vector<VerifiedRouteReadinessParticipant>>::Ok(
        std::move(result));
  } catch (const std::exception &exception) {
    return readiness_error<std::vector<VerifiedRouteReadinessParticipant>>(
        "route_readiness_batch_failed", exception.what());
  }
}

} // namespace manyforge::deployment
