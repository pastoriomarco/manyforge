#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/types/controller_topology.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "stop_fallback_codec_internal.hpp"

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeUnsignedInteger = 9'007'199'254'740'991ULL;
constexpr std::int64_t kMaximumSafeSignedInteger = 9'007'199'254'740'991LL;
constexpr std::size_t kMaximumIdentityBytes = 128U;
constexpr std::size_t kMaximumLogicalSymbolBytes = 256U;
constexpr std::size_t kMaximumResolvedScopeResources = 4096U;
constexpr std::size_t kMaximumJsonDepth = 32U;
constexpr std::size_t kMaximumJsonNodes = 4096U;
constexpr std::size_t kMaximumJsonStringBytes = 65'536U;

class ValidationError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void fail(std::string_view reason) {
  throw ValidationError(std::string(reason));
}

template <typename Value>
[[nodiscard]] Result<Value> reject(std::string code, std::string message, std::string reason) {
  return Result<Value>::Err(
      ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool portable_identifier(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaximumIdentityBytes) {
    return false;
  }
  const auto alphanumeric = [](const unsigned char character) noexcept {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
  };
  if (!alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [&](const char character) {
    return alphanumeric(static_cast<unsigned char>(character)) || character == '.' ||
           character == '_' || character == '-';
  });
}

void require_identifier(std::string_view value, std::string_view reason) {
  if (!portable_identifier(value)) {
    fail(reason);
  }
}

void require_digest(std::string_view value, std::string_view reason) {
  if (!content_identity::is_sha256_digest(value)) {
    fail(reason);
  }
}

void require_logical_symbol(std::string_view value, std::string_view reason) {
  if (value.empty() || value.size() > kMaximumLogicalSymbolBytes || !portable_identifier(value)) {
    fail(reason);
  }
}

[[nodiscard]] std::string retained_store_key_for_digest(std::string_view digest) {
  require_digest(digest, "retained artifact digest is invalid");
  return "sha256/" + std::string(digest.substr(7U));
}

void validate_artifact(const RetainedArtifactRefDTO& artifact) {
  require_identifier(artifact.artifact_role_id.value(), "retained artifact role is invalid");
  const bool media_type_valid =
      !artifact.media_type.empty() && artifact.media_type.size() <= 256U &&
      artifact.media_type.find('/') != std::string::npos &&
      std::all_of(artifact.media_type.begin(), artifact.media_type.end(),
                  [](const unsigned char byte) { return byte >= 0x21U && byte <= 0x7eU; });
  if (!media_type_valid || artifact.byte_size == 0U ||
      artifact.byte_size > kMaximumSafeUnsignedInteger) {
    fail("retained artifact metadata is invalid");
  }
  require_digest(artifact.content_digest.value(), "retained artifact digest is invalid");
  if (artifact.content_addressed_store_key.value() !=
      retained_store_key_for_digest(artifact.content_digest.value())) {
    fail("retained artifact store key does not match its digest");
  }
}

void validate_contract_ref(const ProviderContractRefDTO& reference) {
  require_identifier(reference.provider_contract_role_id.value(),
                     "provider contract role is invalid");
  require_identifier(reference.contract_schema_id.value(),
                     "provider contract schema ID is invalid");
  require_digest(reference.contract_schema_revision.value(),
                 "provider contract schema revision is invalid");
}

[[nodiscard]] bool known_external_kind(const ExternalResourceKind value) {
  return !to_string(value).empty();
}

void validate_runtime_target(const ResolvedStopRuntimeTargetDTO& target) {
  require_identifier(target.provider_instance_id.value(),
                     "runtime target provider instance is invalid");
  require_identifier(target.provider_manifest_id.value(),
                     "runtime target provider manifest ID is invalid");
  require_digest(target.provider_manifest_revision.value(),
                 "runtime target provider manifest revision is invalid");
  require_identifier(target.runtime_unit_id.value(), "runtime target unit ID is invalid");
  require_identifier(target.product_id.value(), "runtime target product ID is invalid");
  require_identifier(target.product_contract_id.value(),
                     "runtime target product contract ID is invalid");
  require_digest(target.product_contract_revision.value(),
                 "runtime target product contract revision is invalid");
  require_digest(target.content_digest.value(), "runtime target product digest is invalid");
  require_identifier(target.provider_runtime_epoch.value(),
                     "runtime target provider epoch is invalid");
  require_identifier(target.runtime_unit_epoch.value(), "runtime target unit epoch is invalid");

  const bool endpoint_present = target.endpoint_role_id.has_value();
  if (endpoint_present != target.logical_endpoint_symbol.has_value()) {
    fail("runtime target endpoint fields are not jointly present");
  }
  if (endpoint_present) {
    require_identifier(target.endpoint_role_id->value(), "runtime target endpoint role is invalid");
    require_logical_symbol(*target.logical_endpoint_symbol,
                           "runtime target endpoint symbol is invalid");
  }

  const std::size_t external_count =
      static_cast<std::size_t>(target.external_resource_alias.has_value()) +
      static_cast<std::size_t>(target.external_resource_kind.has_value()) +
      static_cast<std::size_t>(target.site_overlay_id.has_value()) +
      static_cast<std::size_t>(target.site_overlay_revision.has_value()) +
      static_cast<std::size_t>(target.site_overlay_non_secret_digest.has_value()) +
      static_cast<std::size_t>(target.site_overlay_value_ref.has_value());
  if (external_count != 0U && external_count != 6U) {
    fail("runtime target external-resource fields are not jointly present");
  }
  if (external_count == 6U) {
    if (!endpoint_present || !known_external_kind(*target.external_resource_kind)) {
      fail("external runtime target lacks a valid managed endpoint");
    }
    require_identifier(target.external_resource_alias->value(),
                       "external resource alias is invalid");
    require_identifier(target.site_overlay_id->value(), "site overlay ID is invalid");
    require_digest(target.site_overlay_revision->value(), "site overlay revision is invalid");
    require_digest(target.site_overlay_non_secret_digest->value(),
                   "site overlay non-secret digest is invalid");
    require_identifier(target.site_overlay_value_ref->value(),
                       "site overlay value reference is invalid");
  }
}

void validate_command(const StopContainmentCommandDTO& command) {
  if (to_string(command.command_kind).empty()) {
    fail("containment command kind is invalid");
  }
  const bool requires_contract =
      command.command_kind == StopContainmentCommandKind::kMotionControlledStop ||
      command.command_kind == StopContainmentCommandKind::kSafeOutputCommand ||
      command.command_kind == StopContainmentCommandKind::kLogicalCancel ||
      command.command_kind == StopContainmentCommandKind::kRegistered;
  if (command.provider_contract_ref.has_value() != requires_contract ||
      command.registered_command_kind_id.has_value() !=
          (command.command_kind == StopContainmentCommandKind::kRegistered)) {
    fail("containment command conditional fields are invalid");
  }
  if (command.provider_contract_ref.has_value()) {
    validate_contract_ref(*command.provider_contract_ref);
  }
  if (command.registered_command_kind_id.has_value()) {
    require_identifier(command.registered_command_kind_id->value(),
                       "registered command kind is invalid");
  }
}

[[nodiscard]] bool source_is_one_of(const StopEvidenceSourceKind source,
                                    std::initializer_list<StopEvidenceSourceKind> allowed) {
  return std::find(allowed.begin(), allowed.end(), source) != allowed.end();
}

[[nodiscard]] bool observation_is_one_of(
    const StopEvidenceObservationKind observation,
    std::initializer_list<StopEvidenceObservationKind> allowed) {
  return std::find(allowed.begin(), allowed.end(), observation) != allowed.end();
}

void validate_evidence_compatibility(const StopEvidenceRequirementDTO& requirement) {
  using Evidence = StopEvidenceRequirementKind;
  using Observation = StopEvidenceObservationKind;
  using Source = StopEvidenceSourceKind;
  const auto source = requirement.source_requirement.source_kind;
  const auto observation = requirement.observation_semantics.observation_kind;
  switch (requirement.evidence_kind) {
    case Evidence::kControllerGoalTerminal:
      if (source != Source::kControllerGoalStatus ||
          !observation_is_one_of(observation,
                                 {Observation::kCommandResult, Observation::kPostCommandSample})) {
        fail("controller goal evidence has an incompatible source");
      }
      return;
    case Evidence::kMotionRest:
      if (source != Source::kBindingStateAuthority ||
          observation != Observation::kPostCommandDwell) {
        fail("motion-rest evidence has an incompatible source");
      }
      return;
    case Evidence::kDiscreteAcknowledgement:
      if (source != Source::kCommandTargetStatus ||
          !observation_is_one_of(observation,
                                 {Observation::kCommandResult, Observation::kPostCommandSample})) {
        fail("discrete acknowledgement has an incompatible source");
      }
      return;
    case Evidence::kIoSafeOutput:
      if (!source_is_one_of(source, {Source::kBindingStateAuthority, Source::kProviderEvidence}) ||
          !observation_is_one_of(
              observation, {Observation::kPostCommandSample, Observation::kPostCommandDwell})) {
        fail("safe-output evidence has an incompatible source");
      }
      return;
    case Evidence::kLogicalCancellationNoContinuingEffect:
      if (!source_is_one_of(source, {Source::kCommandTargetStatus, Source::kProviderEvidence}) ||
          !observation_is_one_of(observation,
                                 {Observation::kCommandResult, Observation::kPostCommandSample,
                                  Observation::kPostCommandDwell})) {
        fail("logical-cancellation evidence has an incompatible source");
      }
      return;
    case Evidence::kRegistered:
      if (source != Source::kProviderEvidence) {
        fail("registered evidence lacks a provider source");
      }
      return;
  }
  fail("evidence kind is invalid");
}

void validate_source_requirement(const StopEvidenceSourceRequirementDTO& source) {
  if (to_string(source.source_kind).empty() || source.freshness_budget_ns <= 0 ||
      source.freshness_budget_ns > kMaximumSafeSignedInteger) {
    fail("evidence source requirement is invalid");
  }
  if (source.source_kind == StopEvidenceSourceKind::kBindingStateAuthority) {
    if (!source.endpoint_role_id.has_value() || source.allowed_authority_classes.empty() ||
        source.allowed_authority_classes.size() > 3U) {
      fail("binding-state source requirement is incomplete");
    }
    require_identifier(source.endpoint_role_id->value(), "binding-state endpoint role is invalid");
    std::string_view previous_authority;
    for (const auto authority : source.allowed_authority_classes) {
      const auto spelling = to_string(authority);
      if (spelling.empty() || (!previous_authority.empty() && previous_authority >= spelling)) {
        fail("binding-state authority set is invalid");
      }
      previous_authority = spelling;
    }
    return;
  }
  if (source.source_kind == StopEvidenceSourceKind::kProviderEvidence) {
    if (!source.endpoint_role_id.has_value() || !source.allowed_authority_classes.empty()) {
      fail("provider evidence source requirement is invalid");
    }
    require_identifier(source.endpoint_role_id->value(),
                       "provider evidence endpoint role is invalid");
    return;
  }
  if (source.endpoint_role_id.has_value() || !source.allowed_authority_classes.empty()) {
    fail("status evidence source carries forbidden conditional fields");
  }
}

void validate_declarative_evidence(const StopEvidenceRequirementDTO& requirement) {
  require_identifier(requirement.evidence_item_id.value(), "evidence item ID is invalid");
  if (to_string(requirement.evidence_kind).empty() ||
      to_string(requirement.observation_semantics.observation_kind).empty()) {
    fail("evidence enum value is invalid");
  }
  validate_source_requirement(requirement.source_requirement);
  validate_evidence_compatibility(requirement);
  const bool requires_contract =
      requirement.evidence_kind == StopEvidenceRequirementKind::kMotionRest ||
      requirement.evidence_kind == StopEvidenceRequirementKind::kIoSafeOutput ||
      requirement.evidence_kind ==
          StopEvidenceRequirementKind::kLogicalCancellationNoContinuingEffect ||
      requirement.evidence_kind == StopEvidenceRequirementKind::kRegistered;
  if (requirement.provider_contract_ref.has_value() != requires_contract ||
      requirement.registered_evidence_kind_id.has_value() !=
          (requirement.evidence_kind == StopEvidenceRequirementKind::kRegistered)) {
    fail("evidence conditional fields are invalid");
  }
  if (requirement.provider_contract_ref.has_value()) {
    validate_contract_ref(*requirement.provider_contract_ref);
  }
  if (requirement.registered_evidence_kind_id.has_value()) {
    require_identifier(requirement.registered_evidence_kind_id->value(),
                       "registered evidence kind is invalid");
  }
  const auto dwell = requirement.observation_semantics.minimum_dwell_ns;
  if ((requirement.observation_semantics.observation_kind ==
       StopEvidenceObservationKind::kPostCommandDwell) != dwell.has_value() ||
      (dwell.has_value() && (*dwell <= 0 || *dwell > kMaximumSafeSignedInteger))) {
    fail("evidence dwell field is invalid");
  }
}

void validate_resolved_evidence_source(const StopEvidenceRequirementDTO& requirement,
                                       const ResolvedStopEvidenceSourceDTO& source) {
  if (source.source_kind != requirement.source_requirement.source_kind) {
    fail("resolved evidence source kind does not match its requirement");
  }
  validate_runtime_target(source.runtime_target);
  const bool binding = source.source_kind == StopEvidenceSourceKind::kBindingStateAuthority;
  const bool provider = source.source_kind == StopEvidenceSourceKind::kProviderEvidence;
  if (source.state_authority_id.has_value() != binding ||
      source.state_authority_class.has_value() != binding ||
      source.provider_contract_ref.has_value() != provider) {
    fail("resolved evidence source conditional fields are invalid");
  }
  if (binding) {
    require_identifier(source.state_authority_id->value(),
                       "resolved state authority ID is invalid");
    if (to_string(*source.state_authority_class).empty() ||
        std::find(requirement.source_requirement.allowed_authority_classes.begin(),
                  requirement.source_requirement.allowed_authority_classes.end(),
                  *source.state_authority_class) ==
            requirement.source_requirement.allowed_authority_classes.end()) {
      fail("resolved state authority class was not allowed");
    }
  }
  if (provider) {
    validate_contract_ref(*source.provider_contract_ref);
    if (!requirement.provider_contract_ref.has_value() ||
        *source.provider_contract_ref != *requirement.provider_contract_ref) {
      fail("resolved provider evidence contract does not match");
    }
  }
  if (requirement.source_requirement.endpoint_role_id.has_value()) {
    if (!source.runtime_target.endpoint_role_id.has_value() ||
        *source.runtime_target.endpoint_role_id !=
            *requirement.source_requirement.endpoint_role_id) {
      fail("resolved evidence endpoint does not match its requirement");
    }
  }
}

[[nodiscard]] std::size_t granularity_rank(const ContainmentGranularity granularity) {
  switch (granularity) {
    case ContainmentGranularity::kResource:
      return 0U;
    case ContainmentGranularity::kControllerSet:
      return 1U;
    case ContainmentGranularity::kControlDomain:
      return 2U;
  }
  fail("containment granularity is invalid");
}

[[nodiscard]] ContainmentGranularity scope_granularity(
    const ResolvedStopContainmentScopeDTO& scope) {
  return std::visit(
      [](const auto& row) {
        using Row = std::decay_t<decltype(row)>;
        if constexpr (std::is_same_v<Row, ResolvedStopResourceContainmentScopeDTO>) {
          return ContainmentGranularity::kResource;
        } else if constexpr (std::is_same_v<Row, ResolvedStopControllerSetContainmentScopeDTO>) {
          return ContainmentGranularity::kControllerSet;
        } else {
          return ContainmentGranularity::kControlDomain;
        }
      },
      scope);
}

[[nodiscard]] const ActivationSetId& scope_activation_set(
    const ResolvedStopContainmentScopeDTO& scope) {
  return std::visit([](const auto& row) -> const ActivationSetId& { return row.activation_set_id; },
                    scope);
}

void validate_scope(const ResolvedStopContainmentScopeDTO& scope,
                    const ResolvedStopBasePlanDTO& plan,
                    const ContainmentGranularity expected_granularity) {
  if (scope_granularity(scope) != expected_granularity ||
      scope_activation_set(scope) != plan.activation_set_id) {
    fail("resolved containment scope discriminator or activation set differs");
  }
  require_identifier(scope_activation_set(scope).value(), "scope activation set ID is invalid");
  std::visit(
      [&](const auto& row) {
        using Row = std::decay_t<decltype(row)>;
        if constexpr (std::is_same_v<Row, ResolvedStopResourceContainmentScopeDTO>) {
          if (row.resource_ids.empty() ||
              row.resource_ids.size() > kMaximumResolvedScopeResources ||
              !std::is_sorted(row.resource_ids.begin(), row.resource_ids.end())) {
            fail("resource containment scope is not bounded and canonical");
          }
          for (std::size_t index = 0U; index < row.resource_ids.size(); ++index) {
            require_identifier(row.resource_ids[index].value(), "scope resource ID is invalid");
            if (index != 0U && row.resource_ids[index - 1U] == row.resource_ids[index]) {
              fail("resource containment scope contains a duplicate");
            }
          }
        } else if constexpr (std::is_same_v<Row, ResolvedStopControllerSetContainmentScopeDTO>) {
          const auto less = [](const auto& left, const auto& right) {
            return std::tie(left.controller_role_id, left.logical_controller_symbol) <
                   std::tie(right.controller_role_id, right.logical_controller_symbol);
          };
          if (row.controllers.empty() ||
              row.controllers.size() > kMaxControllerTopologyControllers ||
              !std::is_sorted(row.controllers.begin(), row.controllers.end(), less)) {
            fail("controller containment scope is not bounded and canonical");
          }
          for (std::size_t index = 0U; index < row.controllers.size(); ++index) {
            require_identifier(row.controllers[index].controller_role_id.value(),
                               "scope controller role is invalid");
            require_logical_symbol(row.controllers[index].logical_controller_symbol,
                                   "scope logical controller symbol is invalid");
            if (index != 0U && (!less(row.controllers[index - 1U], row.controllers[index]) ||
                                row.controllers[index - 1U].controller_role_id ==
                                    row.controllers[index].controller_role_id)) {
              fail("controller containment scope contains a duplicate");
            }
          }
        } else {
          if (row.control_domain_id != plan.control_domain_id ||
              row.control_domain_revision != plan.control_domain_revision) {
            fail("domain containment scope does not match the base plan");
          }
          require_identifier(row.control_domain_id.value(), "scope control domain ID is invalid");
          require_digest(row.control_domain_revision.value(),
                         "scope control domain revision is invalid");
        }
      },
      scope);
}

template <typename Row, typename Less>
[[nodiscard]] bool canonical_subset(const std::vector<Row>& left, const std::vector<Row>& right,
                                    Less less) {
  return std::includes(right.begin(), right.end(), left.begin(), left.end(), less);
}

void validate_scope_widening(const ResolvedStopContainmentScopeDTO& previous,
                             const ResolvedStopContainmentScopeDTO& current) {
  const auto previous_rank = granularity_rank(scope_granularity(previous));
  const auto current_rank = granularity_rank(scope_granularity(current));
  if (current_rank < previous_rank) {
    fail("later Stop stage narrows containment granularity");
  }
  if (current_rank != previous_rank) {
    return;
  }
  if (const auto* left = std::get_if<ResolvedStopResourceContainmentScopeDTO>(&previous)) {
    const auto& right = std::get<ResolvedStopResourceContainmentScopeDTO>(current);
    if (!canonical_subset(left->resource_ids, right.resource_ids, std::less<ResourceId>{})) {
      fail("later resource scope drops an earlier resource");
    }
    return;
  }
  if (const auto* left = std::get_if<ResolvedStopControllerSetContainmentScopeDTO>(&previous)) {
    const auto& right = std::get<ResolvedStopControllerSetContainmentScopeDTO>(current);
    const auto less = [](const auto& lhs, const auto& rhs) {
      return std::tie(lhs.controller_role_id, lhs.logical_controller_symbol) <
             std::tie(rhs.controller_role_id, rhs.logical_controller_symbol);
    };
    if (!canonical_subset(left->controllers, right.controllers, less)) {
      fail("later controller scope drops an earlier controller");
    }
    return;
  }
  if (previous != current) {
    fail("control-domain scope changed within one base plan");
  }
}

void validate_substitution(const ControlledStopSubstitutionContractDTO& contract,
                           const StopContainmentCommandDTO& command) {
  validate_contract_ref(contract.provider_contract_ref);
  if (contract.fallback_stop_modes.empty() ||
      !motion_controlled_stop_fallbacks_valid(contract.requested_stop_mode,
                                              contract.fallback_stop_modes) ||
      !command.provider_contract_ref.has_value() ||
      contract.provider_contract_ref != *command.provider_contract_ref) {
    fail("controlled substitution contract does not match its command");
  }
}

void validate_qualification(const ResolvedStopEscalationStageDTO& stage) {
  const auto& value = stage.command_qualification;
  if (to_string(value.stop_action_class).empty()) {
    fail("Stop action class is invalid");
  }
  const auto required_action = [&] {
    switch (stage.stage_kind) {
      case StopEscalationStageKind::kNominal:
      case StopEscalationStageKind::kControlledFallback:
        return StopActionClass::kControlled;
      case StopEscalationStageKind::kSoftwareHard:
        return StopActionClass::kSoftwareHard;
      case StopEscalationStageKind::kSafetySystemRequest:
        return StopActionClass::kSafetySystemRequest;
    }
    fail("Stop stage kind is invalid");
  }();
  if (value.stop_action_class != required_action) {
    fail("Stop stage kind and qualified action class differ");
  }
  if (stage.containment_command.command_kind == StopContainmentCommandKind::kRegistered) {
    const auto* definition = find_registered_stop_command_definition(
        *stage.containment_command.registered_command_kind_id);
    if (definition == nullptr || definition->stop_action_class != required_action) {
      fail("registered Stop command does not match its qualified action class");
    }
  }
  const bool safety = required_action == StopActionClass::kSafetySystemRequest;
  if (value.safety_system_request_action_class.has_value() != safety ||
      value.commissioned_stop_interface_id.has_value() != safety ||
      value.commissioned_stop_interface_revision.has_value() != safety ||
      value.commissioning_evidence_artifact_ref.has_value() != safety) {
    fail("Stop command qualification conditional fields are invalid");
  }
  if (value.controlled_substitution_contract.has_value()) {
    if (stage.stage_kind != StopEscalationStageKind::kNominal ||
        stage.containment_command.command_kind !=
            StopContainmentCommandKind::kMotionControlledStop) {
      fail("controlled substitution is not on the nominal motion stage");
    }
    validate_substitution(*value.controlled_substitution_contract,
                          stage.containment_command);
  }
  if (safety) {
    require_identifier(value.commissioned_stop_interface_id->value(),
                       "commissioned Stop interface ID is invalid");
    require_digest(value.commissioned_stop_interface_revision->value(),
                   "commissioned Stop interface revision is invalid");
    validate_artifact(*value.commissioning_evidence_artifact_ref);
    if (value.commissioning_evidence_artifact_ref->artifact_role_id.value() !=
            kStopInterfaceCommissioningArtifactRole ||
        value.commissioning_evidence_artifact_ref->media_type !=
            kStopInterfaceCommissioningMediaType ||
        stage.containment_command.command_kind != StopContainmentCommandKind::kRegistered ||
        !stage.containment_command.registered_command_kind_id.has_value()) {
      fail("safety-system request qualification is invalid");
    }
    const auto* definition = find_registered_stop_command_definition(
        *stage.containment_command.registered_command_kind_id);
    if (definition == nullptr ||
        definition->stop_action_class != StopActionClass::kSafetySystemRequest ||
        definition->safety_system_request_action_class !=
            value.safety_system_request_action_class) {
      fail("safety-system request action is not registered exactly");
    }
  }
}

void validate_base_identities(const ResolvedStopBasePlanDTO& plan) {
  require_identifier(plan.cell_id.value(), "cell ID is invalid");
  require_digest(plan.retained_bundle_id.value(), "retained bundle ID is invalid");
  require_digest(plan.graph_revision.value(), "graph revision is invalid");
  require_identifier(plan.source_template.product_id.value(),
                     "source template product ID is invalid");
  require_identifier(plan.source_template.product_contract_id.value(),
                     "source template product contract ID is invalid");
  require_digest(plan.source_template.product_contract_revision.value(),
                 "source template product contract revision is invalid");
  validate_artifact(plan.source_template.artifact_ref);
  require_logical_symbol(plan.source_template.logical_stop_symbol,
                         "source template logical Stop symbol is invalid");
  require_identifier(plan.control_domain_id.value(), "control domain ID is invalid");
  require_digest(plan.control_domain_revision.value(), "control domain revision is invalid");
  require_identifier(plan.control_binding_id.value(), "control binding ID is invalid");
  require_digest(plan.control_binding_revision.value(), "control binding revision is invalid");
  require_identifier(plan.controller_topology_id.value(), "controller topology ID is invalid");
  require_digest(plan.topology_revision.value(), "controller topology revision is invalid");
  require_identifier(plan.control_route_id.value(), "control route ID is invalid");
  require_identifier(plan.activation_set_id.value(), "activation set ID is invalid");
  require_identifier(plan.active_control_configuration_id.value(),
                     "active control configuration ID is invalid");
  if (!plan.active_control_configuration_generation.nonzero()) {
    fail("active control configuration generation is zero");
  }
  require_identifier(plan.source_runtime_unit_id.value(), "source runtime unit ID is invalid");
  require_identifier(plan.source_runtime_epoch.value(), "source runtime epoch is invalid");
  if (plan.source_sequence > kMaximumSafeUnsignedInteger) {
    fail("source sequence exceeds the I-JSON safe range");
  }
  require_identifier(plan.stop_evidence_policy_id.value(), "Stop evidence policy ID is invalid");
  require_digest(plan.stop_evidence_policy_revision.value(),
                 "Stop evidence policy revision is invalid");
  if (to_string(plan.containment_granularity).empty()) {
    fail("top-level containment granularity is invalid");
  }
}

void validate_plan(const ResolvedStopBasePlanDTO& plan) {
  validate_base_identities(plan);
  if (plan.gripper_actuation_kind &&
      *plan.gripper_actuation_kind != GripperActuationKind::kPositionCommanded &&
      *plan.gripper_actuation_kind != GripperActuationKind::kPneumatic) {
    fail("resolved Stop plan gripper actuation kind is invalid");
  }
  if (plan.escalation_stages.empty() || plan.escalation_stages.size() > kMaxStopEscalationStages) {
    fail("resolved Stop plan has no bounded escalation chain");
  }
  std::set<std::string> stage_ids;
  std::set<std::string> evidence_ids;
  std::size_t evidence_count = 0U;
  std::size_t software_hard_count = 0U;
  const ResolvedStopContainmentScopeDTO* previous_scope = nullptr;

  for (std::size_t index = 0U; index < plan.escalation_stages.size(); ++index) {
    const auto& stage = plan.escalation_stages[index];
    if (stage.stage_index != index || stage.stage_index > kMaximumSafeUnsignedInteger) {
      fail("resolved Stop stage index does not equal its position");
    }
    require_identifier(stage.stop_escalation_stage_id.value(),
                       "Stop escalation stage ID is invalid");
    if (!stage_ids.insert(stage.stop_escalation_stage_id.value()).second) {
      fail("resolved Stop plan contains a duplicate stage ID");
    }
    if ((index == 0U && stage.stage_kind != StopEscalationStageKind::kNominal) ||
        (index != 0U && stage.stage_kind == StopEscalationStageKind::kNominal)) {
      fail("resolved Stop plan has an invalid nominal stage position");
    }
    if (stage.stage_kind == StopEscalationStageKind::kSoftwareHard) {
      ++software_hard_count;
    }
    if (stage.stage_kind == StopEscalationStageKind::kSafetySystemRequest &&
        index + 1U != plan.escalation_stages.size()) {
      fail("safety-system request is not the final Stop stage");
    }
    if (index != 0U) {
      const auto previous_kind = plan.escalation_stages[index - 1U].stage_kind;
      if ((stage.stage_kind == StopEscalationStageKind::kControlledFallback &&
           previous_kind != StopEscalationStageKind::kNominal &&
           previous_kind != StopEscalationStageKind::kControlledFallback) ||
          (stage.stage_kind == StopEscalationStageKind::kSoftwareHard &&
           previous_kind != StopEscalationStageKind::kNominal &&
           previous_kind != StopEscalationStageKind::kControlledFallback) ||
          (previous_kind == StopEscalationStageKind::kSoftwareHard &&
           stage.stage_kind != StopEscalationStageKind::kSafetySystemRequest) ||
          previous_kind == StopEscalationStageKind::kSafetySystemRequest) {
        fail("resolved Stop stage kinds are misordered");
      }
    }
    validate_command(stage.containment_command);
    validate_runtime_target(stage.command_target);
    if (stage.default_contract_parameters.has_value() !=
        stage.containment_command.provider_contract_ref.has_value()) {
      fail("resolved Stop command parameter presence is invalid");
    }
    validate_qualification(stage);
    validate_scope(stage.containment_scope, plan, stage.containment_granularity);
    if (previous_scope != nullptr) {
      validate_scope_widening(*previous_scope, stage.containment_scope);
    }
    previous_scope = &stage.containment_scope;
    if (stage.evidence_conjunction != StopEvidenceConjunction::kAllOf ||
        stage.evidence_requirements.empty()) {
      fail("resolved Stop stage lacks non-empty all-of evidence");
    }

    for (const auto& evidence : stage.evidence_requirements) {
      ++evidence_count;
      if (evidence_count > kMaxStopEvidencePolicyItems) {
        fail("resolved Stop plan exceeds its evidence bound");
      }
      validate_declarative_evidence(evidence.requirement);
      validate_resolved_evidence_source(evidence.requirement, evidence.resolved_source);
      if (!evidence_ids.insert(evidence.requirement.evidence_item_id.value()).second) {
        fail("resolved Stop plan contains duplicate evidence IDs");
      }
    }
  }
  const std::size_t expected_hard_count =
      plan.gripper_actuation_kind == GripperActuationKind::kPneumatic ? 0U : 1U;
  if (software_hard_count != expected_hard_count || previous_scope == nullptr ||
      scope_granularity(*previous_scope) != plan.containment_granularity) {
    fail("resolved Stop plan lacks its exact hard floor or top scope");
  }
}

[[nodiscard]] CanonicalValue safe_unsigned(const std::uint64_t value) {
  if (value > kMaximumSafeUnsignedInteger) {
    throw content_identity::CanonicalValueError("unsigned integer exceeds the I-JSON safe range");
  }
  return CanonicalValue(static_cast<double>(value));
}

[[nodiscard]] CanonicalValue safe_signed(const std::int64_t value) {
  if (value < -kMaximumSafeSignedInteger || value > kMaximumSafeSignedInteger) {
    throw content_identity::CanonicalValueError("signed integer exceeds the I-JSON safe range");
  }
  return CanonicalValue(static_cast<double>(value));
}

[[nodiscard]] CanonicalValue json_value(const JsonValueDTO& value, const std::size_t depth,
                                        std::size_t& nodes) {
  if (depth > kMaximumJsonDepth || ++nodes > kMaximumJsonNodes) {
    throw content_identity::CanonicalValueError(
        "contract parameters exceed their structural bound");
  }
  return std::visit(
      [&](const auto& item) -> CanonicalValue {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<Item, bool>) {
          return CanonicalValue(item);
        } else if constexpr (std::is_same_v<Item, double>) {
          return CanonicalValue(item);
        } else if constexpr (std::is_same_v<Item, std::string>) {
          if (item.size() > kMaximumJsonStringBytes) {
            throw content_identity::CanonicalValueError(
                "contract parameter string exceeds its bound");
          }
          return CanonicalValue(item);
        } else if constexpr (std::is_same_v<Item, JsonValueDTO::Array>) {
          CanonicalValue::Array result;
          result.reserve(item.size());
          for (const auto& child : item) {
            result.emplace_back(json_value(child, depth + 1U, nodes));
          }
          return CanonicalValue(std::move(result));
        } else {
          CanonicalValue::Object result;
          result.reserve(item.size());
          for (const auto& [name, child] : item) {
            if (name.size() > kMaximumJsonStringBytes) {
              throw content_identity::CanonicalValueError(
                  "contract parameter member name exceeds its bound");
            }
            result.emplace_back(name, json_value(child, depth + 1U, nodes));
          }
          return CanonicalValue(std::move(result));
        }
      },
      value.value);
}

[[nodiscard]] CanonicalValue json_object(const JsonObjectDTO& object) {
  std::size_t nodes = 0U;
  JsonValueDTO wrapper;
  wrapper.value = object;
  return json_value(wrapper, 0U, nodes);
}

[[nodiscard]] CanonicalValue artifact_value(const RetainedArtifactRefDTO& artifact) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(artifact.artifact_role_id.value())},
      {"media_type", CanonicalValue(artifact.media_type)},
      {"byte_size", safe_unsigned(artifact.byte_size)},
      {"content_digest", CanonicalValue(artifact.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(artifact.content_addressed_store_key.value())}});
}

[[nodiscard]] CanonicalValue contract_value(const ProviderContractRefDTO& contract) {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id", CanonicalValue(contract.provider_contract_role_id.value())},
      {"contract_schema_id", CanonicalValue(contract.contract_schema_id.value())},
      {"contract_schema_revision", CanonicalValue(contract.contract_schema_revision.value())}});
}

[[nodiscard]] CanonicalValue command_value(const StopContainmentCommandDTO& command) {
  CanonicalValue::Object command_fields{
      {"command_kind", CanonicalValue(std::string(to_string(command.command_kind)))}};
  if (command.provider_contract_ref.has_value()) {
    command_fields.emplace_back("provider_contract_ref",
                                contract_value(*command.provider_contract_ref));
  }
  if (command.registered_command_kind_id.has_value()) {
    command_fields.emplace_back("registered_command_kind_id",
                                CanonicalValue(command.registered_command_kind_id->value()));
  }
  return CanonicalValue(
      CanonicalValue::Object{{"command", CanonicalValue(std::move(command_fields))}});
}

[[nodiscard]] CanonicalValue runtime_target_value(const ResolvedStopRuntimeTargetDTO& target) {
  CanonicalValue::Object fields{
      {"provider_instance_id", CanonicalValue(target.provider_instance_id.value())},
      {"provider_manifest_id", CanonicalValue(target.provider_manifest_id.value())},
      {"provider_manifest_revision", CanonicalValue(target.provider_manifest_revision.value())},
      {"runtime_unit_id", CanonicalValue(target.runtime_unit_id.value())},
      {"product_id", CanonicalValue(target.product_id.value())},
      {"product_contract_id", CanonicalValue(target.product_contract_id.value())},
      {"product_contract_revision", CanonicalValue(target.product_contract_revision.value())},
      {"content_digest", CanonicalValue(target.content_digest.value())},
      {"provider_runtime_epoch", CanonicalValue(target.provider_runtime_epoch.value())},
      {"runtime_unit_epoch", CanonicalValue(target.runtime_unit_epoch.value())}};
  if (target.endpoint_role_id.has_value()) {
    fields.emplace_back("endpoint_role_id", CanonicalValue(target.endpoint_role_id->value()));
    fields.emplace_back("logical_endpoint_symbol", CanonicalValue(*target.logical_endpoint_symbol));
  }
  if (target.external_resource_alias.has_value()) {
    fields.emplace_back("external_resource_alias",
                        CanonicalValue(target.external_resource_alias->value()));
    fields.emplace_back("external_resource_kind",
                        CanonicalValue(std::string(to_string(*target.external_resource_kind))));
    fields.emplace_back("site_overlay_id", CanonicalValue(target.site_overlay_id->value()));
    fields.emplace_back("site_overlay_revision",
                        CanonicalValue(target.site_overlay_revision->value()));
    fields.emplace_back("site_overlay_non_secret_digest",
                        CanonicalValue(target.site_overlay_non_secret_digest->value()));
    fields.emplace_back("site_overlay_value_ref",
                        CanonicalValue(target.site_overlay_value_ref->value()));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue substitution_value(
    const ControlledStopSubstitutionContractDTO& value) {
  CanonicalValue::Array modes;
  for (const auto mode : value.fallback_stop_modes)
    modes.emplace_back(std::string(to_string(mode)));
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_ref", contract_value(value.provider_contract_ref)},
      {"requested_stop_mode", CanonicalValue(std::string(to_string(value.requested_stop_mode)))},
      {"fallback_stop_modes", CanonicalValue(std::move(modes))}});
}

[[nodiscard]] CanonicalValue qualification_value(const ResolvedStopCommandQualificationDTO& value) {
  CanonicalValue::Object fields{
      {"stop_action_class", CanonicalValue(std::string(to_string(value.stop_action_class)))}};
  if (value.controlled_substitution_contract.has_value()) {
    fields.emplace_back("controlled_substitution_contract",
                        substitution_value(*value.controlled_substitution_contract));
  }
  if (value.safety_system_request_action_class.has_value()) {
    fields.emplace_back(
        "safety_system_request_action_class",
        CanonicalValue(std::string(to_string(*value.safety_system_request_action_class))));
    fields.emplace_back("commissioned_stop_interface_id",
                        CanonicalValue(value.commissioned_stop_interface_id->value()));
    fields.emplace_back("commissioned_stop_interface_revision",
                        CanonicalValue(value.commissioned_stop_interface_revision->value()));
    fields.emplace_back("commissioning_evidence_artifact_ref",
                        artifact_value(*value.commissioning_evidence_artifact_ref));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue scope_value(const ResolvedStopContainmentScopeDTO& scope) {
  return std::visit(
      [](const auto& row) -> CanonicalValue {
        using Row = std::decay_t<decltype(row)>;
        CanonicalValue::Object fields{
            {"activation_set_id", CanonicalValue(row.activation_set_id.value())}};
        if constexpr (std::is_same_v<Row, ResolvedStopResourceContainmentScopeDTO>) {
          fields.emplace_back(
              "containment_granularity",
              CanonicalValue(std::string(to_string(ContainmentGranularity::kResource))));
          CanonicalValue::Array resources;
          resources.reserve(row.resource_ids.size());
          for (const auto& resource : row.resource_ids) {
            resources.emplace_back(resource.value());
          }
          fields.emplace_back("resource_ids", CanonicalValue(std::move(resources)));
        } else if constexpr (std::is_same_v<Row, ResolvedStopControllerSetContainmentScopeDTO>) {
          fields.emplace_back(
              "containment_granularity",
              CanonicalValue(std::string(to_string(ContainmentGranularity::kControllerSet))));
          CanonicalValue::Array controllers;
          controllers.reserve(row.controllers.size());
          for (const auto& controller : row.controllers) {
            controllers.emplace_back(CanonicalValue::Object{
                {"controller_role_id", CanonicalValue(controller.controller_role_id.value())},
                {"logical_controller_symbol",
                 CanonicalValue(controller.logical_controller_symbol)}});
          }
          fields.emplace_back("controllers", CanonicalValue(std::move(controllers)));
        } else {
          fields.emplace_back(
              "containment_granularity",
              CanonicalValue(std::string(to_string(ContainmentGranularity::kControlDomain))));
          fields.emplace_back("control_domain_id", CanonicalValue(row.control_domain_id.value()));
          fields.emplace_back("control_domain_revision",
                              CanonicalValue(row.control_domain_revision.value()));
        }
        return CanonicalValue(std::move(fields));
      },
      scope);
}

[[nodiscard]] CanonicalValue source_requirement_value(
    const StopEvidenceSourceRequirementDTO& source) {
  CanonicalValue::Object fields{
      {"source_kind", CanonicalValue(std::string(to_string(source.source_kind)))},
      {"freshness_budget_ns", safe_signed(source.freshness_budget_ns)}};
  if (source.endpoint_role_id.has_value()) {
    fields.emplace_back(source.source_kind == StopEvidenceSourceKind::kBindingStateAuthority
                            ? "state_endpoint_role_id"
                            : "evidence_endpoint_role_id",
                        CanonicalValue(source.endpoint_role_id->value()));
  }
  if (!source.allowed_authority_classes.empty()) {
    CanonicalValue::Array classes;
    classes.reserve(source.allowed_authority_classes.size());
    for (const auto authority : source.allowed_authority_classes) {
      classes.emplace_back(std::string(to_string(authority)));
    }
    fields.emplace_back("allowed_authority_classes", CanonicalValue(std::move(classes)));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue observation_value(const StopEvidenceObservationDTO& observation) {
  CanonicalValue::Object fields{
      {"observation_kind", CanonicalValue(std::string(to_string(observation.observation_kind)))}};
  if (observation.minimum_dwell_ns.has_value()) {
    fields.emplace_back("minimum_dwell_ns", safe_signed(*observation.minimum_dwell_ns));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue declarative_evidence_value(
    const StopEvidenceRequirementDTO& requirement) {
  CanonicalValue::Object evidence_fields{
      {"evidence_kind", CanonicalValue(std::string(to_string(requirement.evidence_kind)))}};
  if (requirement.provider_contract_ref.has_value()) {
    evidence_fields.emplace_back("provider_contract_ref",
                                 contract_value(*requirement.provider_contract_ref));
  }
  if (requirement.registered_evidence_kind_id.has_value()) {
    evidence_fields.emplace_back("registered_evidence_kind_id",
                                 CanonicalValue(requirement.registered_evidence_kind_id->value()));
  }
  CanonicalValue::Object fields{
      {"evidence_item_id", CanonicalValue(requirement.evidence_item_id.value())},
      {"evidence", CanonicalValue(std::move(evidence_fields))},
      {"source_requirement", source_requirement_value(requirement.source_requirement)},
      {"observation_semantics", observation_value(requirement.observation_semantics)}};
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue resolved_source_value(const ResolvedStopEvidenceSourceDTO& source) {
  CanonicalValue::Object fields{
      {"source_kind", CanonicalValue(std::string(to_string(source.source_kind)))},
      {"runtime_target", runtime_target_value(source.runtime_target)}};
  if (source.state_authority_id.has_value()) {
    fields.emplace_back("state_authority_id", CanonicalValue(source.state_authority_id->value()));
    fields.emplace_back("state_authority_class",
                        CanonicalValue(std::string(to_string(*source.state_authority_class))));
  }
  if (source.provider_contract_ref.has_value()) {
    fields.emplace_back("provider_contract_ref", contract_value(*source.provider_contract_ref));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue resolved_evidence_value(
    const ResolvedStopEvidenceRequirementDTO& evidence) {
  auto fields = declarative_evidence_value(evidence.requirement).as_object();
  fields.emplace_back("resolved_source", resolved_source_value(evidence.resolved_source));
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue stage_value(const ResolvedStopEscalationStageDTO& stage) {
  CanonicalValue::Array evidence;
  evidence.reserve(stage.evidence_requirements.size());
  for (const auto& row : stage.evidence_requirements) {
    evidence.emplace_back(resolved_evidence_value(row));
  }
  CanonicalValue::Object fields{
      {"stage_index", safe_unsigned(stage.stage_index)},
      {"stop_escalation_stage_id", CanonicalValue(stage.stop_escalation_stage_id.value())},
      {"stage_kind", CanonicalValue(std::string(to_string(stage.stage_kind)))},
      {"containment_granularity",
       CanonicalValue(std::string(to_string(stage.containment_granularity)))},
      {"containment_command", command_value(stage.containment_command)},
      {"command_qualification", qualification_value(stage.command_qualification)},
      {"command_target", runtime_target_value(stage.command_target)},
      {"containment_scope", scope_value(stage.containment_scope)},
      {"evidence_conjunction", CanonicalValue(std::string(to_string(stage.evidence_conjunction)))},
      {"evidence_requirements", CanonicalValue(std::move(evidence))}};
  if (stage.default_contract_parameters.has_value()) {
    fields.emplace_back("default_contract_parameters",
                        json_object(*stage.default_contract_parameters));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue template_value(const ResolvedStopTemplateRefDTO& source) {
  return CanonicalValue(CanonicalValue::Object{
      {"product_id", CanonicalValue(source.product_id.value())},
      {"product_contract_id", CanonicalValue(source.product_contract_id.value())},
      {"product_contract_revision", CanonicalValue(source.product_contract_revision.value())},
      {"artifact_ref", artifact_value(source.artifact_ref)},
      {"logical_stop_symbol", CanonicalValue(source.logical_stop_symbol)}});
}

[[nodiscard]] CanonicalValue plan_value(const ResolvedStopBasePlanDTO& plan,
                                        const bool include_own_id) {
  CanonicalValue::Array stages;
  stages.reserve(plan.escalation_stages.size());
  for (const auto& stage : plan.escalation_stages) {
    stages.emplace_back(stage_value(stage));
  }
  CanonicalValue::Object fields{
      {"api_version", CanonicalValue(std::string(kResolvedStopBasePlanApiVersion))},
      {"cell_id", CanonicalValue(plan.cell_id.value())},
      {"retained_bundle_id", CanonicalValue(plan.retained_bundle_id.value())},
      {"graph_revision", CanonicalValue(plan.graph_revision.value())},
      {"source_template", template_value(plan.source_template)},
      {"control_domain_id", CanonicalValue(plan.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(plan.control_domain_revision.value())},
      {"control_binding_id", CanonicalValue(plan.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(plan.control_binding_revision.value())},
      {"controller_topology_id", CanonicalValue(plan.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(plan.topology_revision.value())},
      {"control_route_id", CanonicalValue(plan.control_route_id.value())},
      {"activation_set_id", CanonicalValue(plan.activation_set_id.value())},
      {"active_control_configuration_id",
       CanonicalValue(plan.active_control_configuration_id.value())},
      {"active_control_configuration_generation",
       safe_unsigned(plan.active_control_configuration_generation.value())},
      {"source_runtime_unit_id", CanonicalValue(plan.source_runtime_unit_id.value())},
      {"source_runtime_epoch", CanonicalValue(plan.source_runtime_epoch.value())},
      {"source_sequence", safe_unsigned(plan.source_sequence)},
      {"stop_evidence_policy_id", CanonicalValue(plan.stop_evidence_policy_id.value())},
      {"stop_evidence_policy_revision", CanonicalValue(plan.stop_evidence_policy_revision.value())},
      {"containment_granularity",
       CanonicalValue(std::string(to_string(plan.containment_granularity)))},
      {"escalation_stages", CanonicalValue(std::move(stages))}};
  if (plan.gripper_actuation_kind) {
    fields.emplace_back("gripper_actuation_kind", CanonicalValue(
        *plan.gripper_actuation_kind == GripperActuationKind::kPneumatic
            ? "pneumatic" : "position_commanded"));
  }
  if (include_own_id) {
    fields.emplace_back("resolved_stop_base_plan_id",
                        CanonicalValue(plan.resolved_stop_base_plan_id.value()));
  }
  return detail::stop_fallback_wire_fields(CanonicalValue(std::move(fields)), false);
}

[[nodiscard]] ResolvedStopBasePlanId computed_id(const ResolvedStopBasePlanDTO& plan) {
  return ResolvedStopBasePlanId(content_identity::framed_sha256(kResolvedStopBasePlanIdentityDomain,
                                                                kResolvedStopBasePlanApiVersion,
                                                                plan_value(plan, false)));
}

[[nodiscard]] bool legacy_identity(const ResolvedStopBasePlanDTO& plan) {
  try {
    const auto legacy = detail::stop_fallback_wire_fields(plan_value(plan, false), true);
    const auto id = content_identity::framed_sha256(kResolvedStopBasePlanIdentityDomain,
                                                    kResolvedStopBasePlanApiVersion, legacy);
    return content_identity::digest_equal(id, plan.resolved_stop_base_plan_id.value());
  } catch (const std::invalid_argument&) {
    return false;
  }
}

[[nodiscard]] std::string checked_encoded_bytes(const ResolvedStopBasePlanDTO& plan) {
  auto wire = plan_value(plan, true);
  if (!content_identity::digest_equal(computed_id(plan).value(),
                                      plan.resolved_stop_base_plan_id.value()) &&
      legacy_identity(plan))
    wire = detail::stop_fallback_wire_fields(wire, true);
  auto bytes = content_identity::canonical_json(wire);
  if (bytes.size() > content_identity::kMaximumCanonicalJsonBytes) {
    fail("resolved Stop base-plan bytes exceed the canonical parser ceiling");
  }
  static_cast<void>(content_identity::parse_canonical_json(bytes));
  return bytes;
}

}  // namespace

Result<ResolvedStopBasePlanDTO> MakeResolvedStopBasePlan(ResolvedStopBasePlanDTO plan) {
  try {
    for (auto& stage : plan.escalation_stages) {
      if (stage.default_contract_parameters)
        detail::normalize_stop_fallback_parameters(*stage.default_contract_parameters);
    }
    validate_plan(plan);
    plan.resolved_stop_base_plan_id = computed_id(plan);
    static_cast<void>(checked_encoded_bytes(plan));
    return Result<ResolvedStopBasePlanDTO>::Ok(std::move(plan));
  } catch (const ValidationError& error) {
    return reject<ResolvedStopBasePlanDTO>(
        "resolved_stop_base_plan_invalid",
        "the resolved Stop base plan is not closed and internally consistent", error.what());
  } catch (const std::exception&) {
    return reject<ResolvedStopBasePlanDTO>(
        "resolved_stop_base_plan_not_canonical",
        "the resolved Stop base plan cannot be represented canonically", "canonicalization_failed");
  }
}

Result<void> VerifyResolvedStopBasePlan(const ResolvedStopBasePlanDTO& plan) {
  try {
    validate_plan(plan);
    if (!content_identity::is_sha256_digest(plan.resolved_stop_base_plan_id.value()) ||
        (!content_identity::digest_equal(computed_id(plan).value(),
                                         plan.resolved_stop_base_plan_id.value()) &&
         !legacy_identity(plan))) {
      return Result<void>::Err(
          ErrorDTO{"resolved_stop_base_plan_id_mismatch",
                   "the resolved Stop base-plan ID does not match its canonical content",
                   {{"reason", "resolved_stop_base_plan_id_mismatch"}}});
    }
    static_cast<void>(checked_encoded_bytes(plan));
    return Result<void>::Ok();
  } catch (const ValidationError& error) {
    return Result<void>::Err(
        ErrorDTO{"resolved_stop_base_plan_invalid",
                 "the resolved Stop base plan is not closed and internally consistent",
                 {{"reason", error.what()}}});
  } catch (const std::exception&) {
    return Result<void>::Err(
        ErrorDTO{"resolved_stop_base_plan_not_canonical",
                 "the resolved Stop base plan cannot be represented canonically",
                 {{"reason", "canonicalization_failed"}}});
  }
}

Result<std::string> EncodeResolvedStopBasePlan(const ResolvedStopBasePlanDTO& plan) {
  const auto verified = VerifyResolvedStopBasePlan(plan);
  if (!verified.ok()) {
    return Result<std::string>::Err(verified.error());
  }
  try {
    return Result<std::string>::Ok(checked_encoded_bytes(plan));
  } catch (const std::exception&) {
    return reject<std::string>("resolved_stop_base_plan_not_canonical",
                               "the resolved Stop base plan cannot be encoded canonically",
                               "canonical_encoding_failed");
  }
}

}  // namespace manyforge::core
