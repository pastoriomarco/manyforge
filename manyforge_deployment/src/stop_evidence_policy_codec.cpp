#include "manyforge_deployment/stop_evidence_policy_codec.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "catalog_record_referent_internal.hpp"
#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw StopEvidencePolicyCodecError(std::move(message));
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue& object, std::string_view name) {
  const auto* member = object.find_object_member(name);
  if (member == nullptr) {
    fail("stop-evidence policy normalized value is missing " +
         std::string(name));
  }
  return *member;
}

[[nodiscard]] const std::string& require_string(
    const CanonicalValue& value, std::string_view name) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail("stop-evidence policy normalized field is not a string: " +
         std::string(name));
  }
  return *text;
}

[[nodiscard]] std::int64_t require_integer(const CanonicalValue& value,
                                           std::string_view name) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) ||
      std::trunc(*number) != *number || *number < 0.0 ||
      *number > static_cast<double>(
                    core::kMaxStopEvidencePolicyDurationNs)) {
    fail("stop-evidence policy normalized field is not a bounded integer: " +
         std::string(name));
  }
  return static_cast<std::int64_t>(*number);
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view name) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail("stop-evidence policy normalized field is not an array: " +
         std::string(name));
  }
  return *array;
}

[[nodiscard]] core::ContainmentGranularity parse_granularity(
    std::string_view token) {
  if (token == "resource") {
    return core::ContainmentGranularity::kResource;
  }
  if (token == "controller_set") {
    return core::ContainmentGranularity::kControllerSet;
  }
  if (token == "control_domain") {
    return core::ContainmentGranularity::kControlDomain;
  }
  fail("unknown containment_granularity");
}

[[nodiscard]] core::StopContainmentCommandKind parse_command_kind(
    std::string_view token) {
  if (token == "controller_goal_cancel") {
    return core::StopContainmentCommandKind::kControllerGoalCancel;
  }
  if (token == "motion_controlled_stop") {
    return core::StopContainmentCommandKind::kMotionControlledStop;
  }
  if (token == "capability_command_cancel") {
    return core::StopContainmentCommandKind::kCapabilityCommandCancel;
  }
  if (token == "safe_output_command") {
    return core::StopContainmentCommandKind::kSafeOutputCommand;
  }
  if (token == "logical_cancel") {
    return core::StopContainmentCommandKind::kLogicalCancel;
  }
  if (token == "registered") {
    return core::StopContainmentCommandKind::kRegistered;
  }
  fail("unknown containment command kind");
}

[[nodiscard]] core::StopEvidenceRequirementKind parse_evidence_kind(
    std::string_view token) {
  if (token == "controller_goal_terminal") {
    return core::StopEvidenceRequirementKind::kControllerGoalTerminal;
  }
  if (token == "motion_rest") {
    return core::StopEvidenceRequirementKind::kMotionRest;
  }
  if (token == "discrete_acknowledgement") {
    return core::StopEvidenceRequirementKind::kDiscreteAcknowledgement;
  }
  if (token == "io_safe_output") {
    return core::StopEvidenceRequirementKind::kIoSafeOutput;
  }
  if (token == "logical_cancellation_no_continuing_effect") {
    return core::StopEvidenceRequirementKind::
        kLogicalCancellationNoContinuingEffect;
  }
  if (token == "registered") {
    return core::StopEvidenceRequirementKind::kRegistered;
  }
  fail("unknown Stop evidence kind");
}

[[nodiscard]] core::StopEvidenceSourceKind parse_source_kind(
    std::string_view token) {
  if (token == "controller_goal_status") {
    return core::StopEvidenceSourceKind::kControllerGoalStatus;
  }
  if (token == "binding_state_authority") {
    return core::StopEvidenceSourceKind::kBindingStateAuthority;
  }
  if (token == "command_target_status") {
    return core::StopEvidenceSourceKind::kCommandTargetStatus;
  }
  if (token == "provider_evidence") {
    return core::StopEvidenceSourceKind::kProviderEvidence;
  }
  fail("unknown Stop evidence source kind");
}

[[nodiscard]] core::StopEvidenceObservationKind parse_observation_kind(
    std::string_view token) {
  if (token == "command_result") {
    return core::StopEvidenceObservationKind::kCommandResult;
  }
  if (token == "post_command_sample") {
    return core::StopEvidenceObservationKind::kPostCommandSample;
  }
  if (token == "post_command_dwell") {
    return core::StopEvidenceObservationKind::kPostCommandDwell;
  }
  fail("unknown Stop evidence observation kind");
}

[[nodiscard]] core::StateAuthorityClass parse_authority_class(
    std::string_view token) {
  if (token == "measured") {
    return core::StateAuthorityClass::kMeasured;
  }
  if (token == "commanded") {
    return core::StateAuthorityClass::kCommanded;
  }
  if (token == "predicted") {
    return core::StateAuthorityClass::kPredicted;
  }
  fail("unknown state-authority class");
}

[[nodiscard]] core::ProviderContractRefDTO decode_provider_contract_ref(
    const CanonicalValue& value) {
  return core::ProviderContractRefDTO{
      core::ProviderContractRoleId(require_string(
          require_member(value, "provider_contract_role_id"),
          "provider_contract_role_id")),
      core::SchemaId(require_string(require_member(value, "contract_schema_id"),
                                    "contract_schema_id")),
      core::SchemaRevision(require_string(
          require_member(value, "contract_schema_revision"),
          "contract_schema_revision")),
  };
}

[[nodiscard]] core::StopContainmentCommandDTO decode_command(
    const CanonicalValue& value) {
  const auto& command = require_member(value, "command");
  const auto kind = parse_command_kind(
      require_string(require_member(command, "command_kind"), "command_kind"));

  std::optional<core::ProviderContractRefDTO> contract;
  std::optional<core::RegisteredStopCommandKindId> registered_kind;
  if (kind == core::StopContainmentCommandKind::kMotionControlledStop ||
      kind == core::StopContainmentCommandKind::kSafeOutputCommand ||
      kind == core::StopContainmentCommandKind::kLogicalCancel ||
      kind == core::StopContainmentCommandKind::kRegistered) {
    contract = decode_provider_contract_ref(
        require_member(command, "provider_contract_ref"));
  }
  if (kind == core::StopContainmentCommandKind::kRegistered) {
    registered_kind = core::RegisteredStopCommandKindId(require_string(
        require_member(command, "registered_command_kind_id"),
        "registered_command_kind_id"));
  }
  return core::StopContainmentCommandDTO{
      core::StopBudgetPhaseId(require_string(
          require_member(value, "command_phase_id"), "command_phase_id")),
      kind,
      std::move(contract),
      std::move(registered_kind),
  };
}

[[nodiscard]] core::StopDeadlineBudgetDTO decode_budget(
    const CanonicalValue& value) {
  const auto& items = require_array(require_member(value, "phases"), "phases");
  std::vector<core::StopBudgetPhaseDTO> phases;
  phases.reserve(items.size());
  for (const auto& item : items) {
    phases.push_back(core::StopBudgetPhaseDTO{
        core::StopBudgetPhaseId(
            require_string(require_member(item, "phase_id"), "phase_id")),
        require_integer(require_member(item, "deadline_offset_ns"),
                        "deadline_offset_ns"),
    });
  }
  return core::StopDeadlineBudgetDTO{
      require_integer(require_member(value, "outer_duration_ns"),
                      "outer_duration_ns"),
      require_integer(require_member(value, "kernel_completion_reserve_ns"),
                      "kernel_completion_reserve_ns"),
      std::move(phases),
  };
}

[[nodiscard]] core::StopEvidenceSourceRequirementDTO decode_source(
    const CanonicalValue& value) {
  const auto kind = parse_source_kind(
      require_string(require_member(value, "source_kind"), "source_kind"));
  std::optional<core::EndpointRoleId> endpoint;
  std::vector<core::StateAuthorityClass> authority_classes;
  if (kind == core::StopEvidenceSourceKind::kBindingStateAuthority) {
    endpoint = core::EndpointRoleId(require_string(
        require_member(value, "state_endpoint_role_id"),
        "state_endpoint_role_id"));
    const auto& items = require_array(
        require_member(value, "allowed_authority_classes"),
        "allowed_authority_classes");
    authority_classes.reserve(items.size());
    for (const auto& item : items) {
      authority_classes.push_back(
          parse_authority_class(require_string(item, "authority_class")));
    }
  } else if (kind == core::StopEvidenceSourceKind::kProviderEvidence) {
    endpoint = core::EndpointRoleId(require_string(
        require_member(value, "evidence_endpoint_role_id"),
        "evidence_endpoint_role_id"));
  }
  return core::StopEvidenceSourceRequirementDTO{
      kind,
      require_integer(require_member(value, "freshness_budget_ns"),
                      "freshness_budget_ns"),
      std::move(endpoint),
      std::move(authority_classes),
  };
}

[[nodiscard]] core::StopEvidenceObservationDTO decode_observation(
    const CanonicalValue& value) {
  const auto kind = parse_observation_kind(require_string(
      require_member(value, "observation_kind"), "observation_kind"));
  std::optional<std::int64_t> dwell;
  if (kind == core::StopEvidenceObservationKind::kPostCommandDwell) {
    dwell = require_integer(require_member(value, "minimum_dwell_ns"),
                            "minimum_dwell_ns");
  }
  return core::StopEvidenceObservationDTO{kind, dwell};
}

[[nodiscard]] core::StopEvidenceRequirementDTO decode_evidence_requirement(
    const CanonicalValue& value) {
  const auto& evidence = require_member(value, "evidence");
  const auto kind = parse_evidence_kind(require_string(
      require_member(evidence, "evidence_kind"), "evidence_kind"));
  std::optional<core::ProviderContractRefDTO> contract;
  std::optional<core::RegisteredStopEvidenceKindId> registered_kind;
  if (kind == core::StopEvidenceRequirementKind::kMotionRest ||
      kind == core::StopEvidenceRequirementKind::kIoSafeOutput ||
      kind == core::StopEvidenceRequirementKind::
                  kLogicalCancellationNoContinuingEffect ||
      kind == core::StopEvidenceRequirementKind::kRegistered) {
    contract = decode_provider_contract_ref(
        require_member(evidence, "provider_contract_ref"));
  }
  if (kind == core::StopEvidenceRequirementKind::kRegistered) {
    registered_kind = core::RegisteredStopEvidenceKindId(require_string(
        require_member(evidence, "registered_evidence_kind_id"),
        "registered_evidence_kind_id"));
  }
  return core::StopEvidenceRequirementDTO{
      core::StopEvidenceItemId(require_string(
          require_member(value, "evidence_item_id"), "evidence_item_id")),
      core::StopBudgetPhaseId(
          require_string(require_member(value, "phase_id"), "phase_id")),
      kind,
      std::move(contract),
      std::move(registered_kind),
      decode_source(require_member(value, "source_requirement")),
      decode_observation(require_member(value, "observation_semantics")),
  };
}

[[nodiscard]] bool source_is_one_of(
    core::StopEvidenceSourceKind source,
    std::initializer_list<core::StopEvidenceSourceKind> allowed) {
  for (const auto candidate : allowed) {
    if (source == candidate) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool observation_is_one_of(
    core::StopEvidenceObservationKind observation,
    std::initializer_list<core::StopEvidenceObservationKind> allowed) {
  for (const auto candidate : allowed) {
    if (observation == candidate) {
      return true;
    }
  }
  return false;
}

void validate_evidence_compatibility(
    const core::StopEvidenceRequirementDTO& requirement) {
  using Evidence = core::StopEvidenceRequirementKind;
  using Observation = core::StopEvidenceObservationKind;
  using Source = core::StopEvidenceSourceKind;
  const auto source = requirement.source_requirement.source_kind;
  const auto observation = requirement.observation_semantics.observation_kind;

  switch (requirement.evidence_kind) {
    case Evidence::kControllerGoalTerminal:
      if (source != Source::kControllerGoalStatus ||
          !observation_is_one_of(
              observation,
              {Observation::kCommandResult,
               Observation::kPostCommandSample})) {
        fail("controller_goal_terminal has an incompatible source or "
             "observation");
      }
      break;
    case Evidence::kMotionRest:
      if (source != Source::kBindingStateAuthority ||
          observation != Observation::kPostCommandDwell) {
        fail("motion_rest requires binding-state post-command dwell");
      }
      break;
    case Evidence::kDiscreteAcknowledgement:
      if (source != Source::kCommandTargetStatus ||
          !observation_is_one_of(
              observation,
              {Observation::kCommandResult,
               Observation::kPostCommandSample})) {
        fail("discrete_acknowledgement has an incompatible source or "
             "observation");
      }
      break;
    case Evidence::kIoSafeOutput:
      if (!source_is_one_of(
              source,
              {Source::kBindingStateAuthority, Source::kProviderEvidence}) ||
          !observation_is_one_of(
              observation,
              {Observation::kPostCommandSample,
               Observation::kPostCommandDwell})) {
        fail("io_safe_output has an incompatible source or observation");
      }
      break;
    case Evidence::kLogicalCancellationNoContinuingEffect:
      if (!source_is_one_of(
              source,
              {Source::kCommandTargetStatus, Source::kProviderEvidence}) ||
          !observation_is_one_of(
              observation,
              {Observation::kCommandResult,
               Observation::kPostCommandSample,
               Observation::kPostCommandDwell})) {
        fail("logical cancellation has an incompatible source or observation");
      }
      break;
    case Evidence::kRegistered:
      if (source != Source::kProviderEvidence) {
        fail("registered Stop evidence requires a provider-evidence source");
      }
      break;
  }
}

void validate_source_shape(
    const core::StopEvidenceSourceRequirementDTO& source) {
  if (source.freshness_budget_ns <= 0) {
    fail("Stop evidence freshness budget must be positive");
  }
  if (source.source_kind ==
      core::StopEvidenceSourceKind::kBindingStateAuthority) {
    if (!source.endpoint_role_id.has_value() ||
        source.allowed_authority_classes.empty()) {
      fail("binding-state source requires endpoint and authority classes");
    }
    std::set<core::StateAuthorityClass> unique;
    for (const auto value : source.allowed_authority_classes) {
      if (!unique.insert(value).second) {
        fail("binding-state source repeats an authority class");
      }
    }
    return;
  }
  if (source.source_kind == core::StopEvidenceSourceKind::kProviderEvidence) {
    if (!source.endpoint_role_id.has_value() ||
        !source.allowed_authority_classes.empty()) {
      fail("provider-evidence source has an invalid conditional field");
    }
    return;
  }
  if (source.endpoint_role_id.has_value() ||
      !source.allowed_authority_classes.empty()) {
    fail("status source has an invalid endpoint or authority-class field");
  }
}

void validate_policy_semantics(const core::StopEvidencePolicyDTO& policy) {
  if (policy.disposition == core::StopEvidenceDisposition::kNotStoppable) {
    if (policy.containment_command.has_value() ||
        policy.deadline_budget.has_value() ||
        policy.evidence_conjunction.has_value() ||
        !policy.evidence_requirements.empty()) {
      fail("not_stoppable policy carries stoppable-only fields");
    }
    return;
  }

  if (!policy.containment_command.has_value() ||
      !policy.deadline_budget.has_value() ||
      policy.evidence_conjunction != core::StopEvidenceConjunction::kAllOf ||
      policy.evidence_requirements.empty()) {
    fail("stoppable policy is missing command, budget, or terminal evidence");
  }
  const auto& command = *policy.containment_command;
  const auto& budget = *policy.deadline_budget;
  if (budget.phases.empty() ||
      budget.phases.size() > core::kMaxStopEvidencePolicyItems ||
      policy.evidence_requirements.size() >
          core::kMaxStopEvidencePolicyItems) {
    fail("Stop policy exceeds its fixed phase/evidence bound");
  }
  if (budget.outer_duration_ns <= 0 ||
      budget.kernel_completion_reserve_ns <= 0) {
    fail("Stop outer duration and Kernel completion reserve must be positive");
  }

  const bool command_requires_contract =
      command.command_kind ==
          core::StopContainmentCommandKind::kMotionControlledStop ||
      command.command_kind ==
          core::StopContainmentCommandKind::kSafeOutputCommand ||
      command.command_kind == core::StopContainmentCommandKind::kLogicalCancel ||
      command.command_kind == core::StopContainmentCommandKind::kRegistered;
  if (command.provider_contract_ref.has_value() !=
      command_requires_contract) {
    fail("containment command has an invalid provider-contract presence");
  }
  if (command.registered_command_kind_id.has_value() !=
      (command.command_kind ==
       core::StopContainmentCommandKind::kRegistered)) {
    fail("containment command has an invalid registered-kind presence");
  }

  std::set<std::string> phase_ids;
  std::int64_t previous_offset = 0;
  for (const auto& phase : budget.phases) {
    if (!phase_ids.insert(phase.phase_id.value()).second) {
      fail("Stop deadline budget contains a duplicate phase ID");
    }
    if (phase.deadline_offset_ns <= previous_offset) {
      fail("Stop phase deadline offsets are not strictly increasing");
    }
    previous_offset = phase.deadline_offset_ns;
  }
  if (previous_offset >
      budget.outer_duration_ns - budget.kernel_completion_reserve_ns) {
    fail("last Stop phase leaves less than the Kernel completion reserve");
  }
  if (command.command_phase_id != budget.phases.front().phase_id) {
    fail("containment command does not use the first Stop phase");
  }

  std::set<std::string> evidence_ids;
  std::size_t last_phase_index = 0U;
  for (const auto& requirement : policy.evidence_requirements) {
    if (!evidence_ids.insert(requirement.evidence_item_id.value()).second) {
      fail("Stop policy contains a duplicate evidence-item ID");
    }
    validate_source_shape(requirement.source_requirement);
    validate_evidence_compatibility(requirement);

    const bool evidence_requires_contract =
        requirement.evidence_kind ==
            core::StopEvidenceRequirementKind::kMotionRest ||
        requirement.evidence_kind ==
            core::StopEvidenceRequirementKind::kIoSafeOutput ||
        requirement.evidence_kind ==
            core::StopEvidenceRequirementKind::
                kLogicalCancellationNoContinuingEffect ||
        requirement.evidence_kind ==
            core::StopEvidenceRequirementKind::kRegistered;
    if (requirement.provider_contract_ref.has_value() !=
        evidence_requires_contract) {
      fail("Stop evidence has an invalid provider-contract presence");
    }
    if (requirement.registered_evidence_kind_id.has_value() !=
        (requirement.evidence_kind ==
         core::StopEvidenceRequirementKind::kRegistered)) {
      fail("Stop evidence has an invalid registered-kind presence");
    }

    std::optional<std::size_t> phase_index;
    for (std::size_t index = 0U; index < budget.phases.size(); ++index) {
      if (budget.phases[index].phase_id == requirement.phase_id) {
        phase_index = index;
        break;
      }
    }
    if (!phase_index.has_value()) {
      fail("Stop evidence references an unknown phase");
    }
    const auto observation =
        requirement.observation_semantics.observation_kind;
    if (observation == core::StopEvidenceObservationKind::kCommandResult) {
      if (*phase_index != 0U ||
          requirement.observation_semantics.minimum_dwell_ns.has_value()) {
        fail("command-result evidence must use the command phase");
      }
    } else {
      if (*phase_index == 0U) {
        fail("post-command evidence must use a later phase");
      }
      if (observation ==
          core::StopEvidenceObservationKind::kPostCommandDwell) {
        if (!requirement.observation_semantics.minimum_dwell_ns.has_value() ||
            *requirement.observation_semantics.minimum_dwell_ns <= 0) {
          fail("post-command dwell must be positive");
        }
        const auto available =
            budget.phases[*phase_index].deadline_offset_ns -
            budget.phases.front().deadline_offset_ns;
        if (*requirement.observation_semantics.minimum_dwell_ns > available) {
          fail("post-command dwell exceeds its static phase-offset window");
        }
      } else if (requirement.observation_semantics.minimum_dwell_ns
                     .has_value()) {
        fail("non-dwell observation carries minimum_dwell_ns");
      }
    }

    if (*phase_index < last_phase_index ||
        *phase_index > last_phase_index + 1U) {
      fail("Stop evidence order does not follow the phase sequence");
    }
    last_phase_index = *phase_index;
  }
  if (last_phase_index + 1U != budget.phases.size()) {
    fail("Stop deadline budget contains an unused phase");
  }
}

[[nodiscard]] core::StopEvidencePolicyDTO decode_normalized(
    const CanonicalValue& normalized) {
  const auto disposition_token = require_string(
      require_member(normalized, "disposition"), "disposition");
  const auto disposition =
      disposition_token == "stoppable"
          ? core::StopEvidenceDisposition::kStoppable
          : core::StopEvidenceDisposition::kNotStoppable;

  core::StopEvidencePolicyDTO policy{
      core::StopEvidencePolicyId(require_string(
          require_member(normalized, "stop_evidence_policy_id"),
          "stop_evidence_policy_id")),
      core::StopEvidencePolicyRevision(require_string(
          require_member(normalized, "stop_evidence_policy_revision"),
          "stop_evidence_policy_revision")),
      disposition,
      parse_granularity(require_string(
          require_member(normalized, "containment_granularity"),
          "containment_granularity")),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      {},
  };
  if (disposition == core::StopEvidenceDisposition::kStoppable) {
    policy.containment_command =
        decode_command(require_member(normalized, "containment_command"));
    policy.deadline_budget =
        decode_budget(require_member(normalized, "deadline_budget"));
    policy.evidence_conjunction = core::StopEvidenceConjunction::kAllOf;
    const auto& items = require_array(
        require_member(normalized, "evidence_requirements"),
        "evidence_requirements");
    policy.evidence_requirements.reserve(items.size());
    for (const auto& item : items) {
      policy.evidence_requirements.push_back(
          decode_evidence_requirement(item));
    }
  }
  validate_policy_semantics(policy);
  return policy;
}

}  // namespace

QualifiedStopEvidencePolicy resolve_and_qualify_stop_evidence_policy(
    const core::StopEvidencePolicyCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes) {
  UnqualifiedSchemaNormalizedArtifactCandidate normalized =
      [&reference, &resolved_store_key, stored_bytes]() {
        try {
          return detail::resolve_and_normalize_exact_catalog_referent(
              reference, resolved_store_key, stored_bytes,
              core::kStopEvidencePolicyApiVersion, "stop-evidence policy");
        } catch (const detail::ExactCatalogReferentError& error) {
          fail(error.what());
        }
      }();

  const detail::ContentIdentitySpec identity_spec{
      detail::catalog_record_identity_domain("stop_evidence_policy"),
      std::string(core::kStopEvidencePolicyApiVersion),
      "stop_evidence_policy_revision",
  };
  if (!detail::verify_content_identity(normalized.normalized_root(),
                                       identity_spec)) {
    fail("stop_evidence_policy_revision does not match normalized semantic "
         "content");
  }

  auto decoded = decode_normalized(normalized.normalized_root());
  if (decoded.stop_evidence_policy_id != reference.record_id ||
      decoded.stop_evidence_policy_revision != reference.record_revision) {
    fail("stop-evidence policy typed ID/revision selector does not match its "
         "referent");
  }
  return QualifiedStopEvidencePolicy(std::move(decoded),
                                     normalized.normalized_root());
}

}  // namespace manyforge::deployment
