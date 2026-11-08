#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/participant_stop_stage.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumIJsonInteger = 9'007'199'254'740'991ULL;

[[nodiscard]] ErrorDTO invalid_error(std::string reason) {
  return ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                  "participant Stop-stage value is not an exact closed value",
                  {{"reason", std::move(reason)}}};
}

[[nodiscard]] Result<void> invalid(std::string reason) {
  return Result<void>::Err(invalid_error(std::move(reason)));
}

template <typename Value>
[[nodiscard]] Result<Value> invalid_value(std::string reason) {
  return Result<Value>::Err(invalid_error(std::move(reason)));
}

template <typename StrongId>
[[nodiscard]] bool identity(const StrongId& value) noexcept {
  return !value.empty() && value.value().size() <= kMaximumStopIdentityBytes &&
         value.value().find('\0') == std::string::npos;
}

template <typename Counter>
[[nodiscard]] bool counter(const Counter& value) noexcept {
  return value.nonzero() && value.value() <= kMaximumIJsonInteger;
}

[[nodiscard]] bool safe_sequence(std::uint64_t value) noexcept {
  return value != 0U && value <= kMaximumIJsonInteger;
}

[[nodiscard]] bool stable_error(const ErrorDTO& value) noexcept {
  if (value.code.empty() || value.code.size() > 128U ||
      value.code.find('\0') != std::string::npos || value.message.empty() ||
      value.message.size() > 1024U || value.message.find('\0') != std::string::npos ||
      value.details.size() > 8U) {
    return false;
  }
  return std::all_of(value.details.begin(), value.details.end(), [](const auto& detail) {
    return !detail.first.empty() && detail.first.size() <= 64U &&
           detail.first.find('\0') == std::string::npos && detail.second.size() <= 256U &&
           detail.second.find('\0') == std::string::npos;
  });
}

[[nodiscard]] std::size_t containment_rank(ContainmentGranularity value) noexcept {
  switch (value) {
    case ContainmentGranularity::kResource:
      return 0U;
    case ContainmentGranularity::kControllerSet:
      return 1U;
    case ContainmentGranularity::kControlDomain:
      return 2U;
  }
  return 3U;
}

template <typename ProviderPath>
[[nodiscard]] bool target_matches_provider(const ResolvedStopRuntimeTargetDTO& target,
                                           const ProviderPath& provider) noexcept {
  return target.provider_instance_id == provider.provider_instance_id &&
         target.provider_manifest_id == provider.provider_manifest_id &&
         target.provider_manifest_revision == provider.provider_manifest_revision &&
         target.runtime_unit_id == provider.provider_runtime_unit_id &&
         target.provider_runtime_epoch == provider.provider_runtime_epoch;
}

[[nodiscard]] bool known(ContainmentGranularity value) noexcept {
  switch (value) {
    case ContainmentGranularity::kResource:
    case ContainmentGranularity::kControllerSet:
    case ContainmentGranularity::kControlDomain:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(StopStageAttemptDispositionDTO value) noexcept {
  switch (value) {
    case StopStageAttemptDispositionDTO::kNotDispatched:
    case StopStageAttemptDispositionDTO::kDispatched:
    case StopStageAttemptDispositionDTO::kEffectUncertain:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(ParticipantStopStageOutcomeDTO value) noexcept {
  switch (value) {
    case ParticipantStopStageOutcomeDTO::kSucceeded:
    case ParticipantStopStageOutcomeDTO::kPartial:
    case ParticipantStopStageOutcomeDTO::kFailed:
    case ParticipantStopStageOutcomeDTO::kSupersededNoEffect:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(StopEvidenceItemStateDTO value) noexcept {
  switch (value) {
    case StopEvidenceItemStateDTO::kRequiredPending:
    case StopEvidenceItemStateDTO::kSatisfied:
    case StopEvidenceItemStateDTO::kMissing:
    case StopEvidenceItemStateDTO::kFailed:
    case StopEvidenceItemStateDTO::kInapplicable:
      return true;
  }
  return false;
}

[[nodiscard]] bool known(MotionControlledStopMode value) noexcept {
  switch (value) {
    case MotionControlledStopMode::kSoftOnPath:
    case MotionControlledStopMode::kSpringBackHold:
    case MotionControlledStopMode::kHardDeceleration:
    case MotionControlledStopMode::kSpringBackFast:
      return true;
  }
  return false;
}

[[nodiscard]] const PreparedStopStageCommandDTO* selected_command(
    const ResolvedParticipantStopStageDTO& stage) noexcept {
  if (stage.stage_index >= stage.participant_stop_plan.stage_commands.size()) {
    return nullptr;
  }
  return &stage.participant_stop_plan.stage_commands[stage.stage_index];
}

[[nodiscard]] CanonicalValue safe_unsigned(std::uint64_t value) {
  if (value > kMaximumIJsonInteger) {
    throw std::invalid_argument("I-JSON integer overflow");
  }
  return CanonicalValue(static_cast<double>(value));
}

[[nodiscard]] CanonicalValue provider_contract_value(const ProviderContractRefDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id", CanonicalValue(value.provider_contract_role_id.value())},
      {"contract_schema_id", CanonicalValue(value.contract_schema_id.value())},
      {"contract_schema_revision", CanonicalValue(value.contract_schema_revision.value())}});
}

[[nodiscard]] CanonicalValue containment_command_value(const StopContainmentCommandDTO& value) {
  CanonicalValue::Object fields{
      {"command_kind", CanonicalValue(std::string(to_string(value.command_kind)))}};
  if (value.provider_contract_ref.has_value()) {
    fields.emplace_back("provider_contract_ref",
                        provider_contract_value(*value.provider_contract_ref));
  }
  if (value.registered_command_kind_id.has_value()) {
    fields.emplace_back("registered_command_kind_id",
                        CanonicalValue(value.registered_command_kind_id->value()));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue digest_projection(const ResolvedParticipantStopStageDTO& stage) {
  const auto* command = selected_command(stage);
  if (command == nullptr ||
      stage.capability_provider_binding_id.has_value() !=
          stage.capability_provider_binding_revision.has_value() ||
      to_string(stage.containment_granularity).empty() ||
      to_string(command->resolved_stage.stage_kind).empty() ||
      to_string(command->resolved_stage.containment_command.command_kind).empty()) {
    throw std::invalid_argument("selected stage is not known");
  }
  CanonicalValue::Array resources;
  resources.reserve(stage.resources.size());
  for (const auto& resource : stage.resources) {
    resources.emplace_back(resource.value());
  }
  CanonicalValue::Object fields{
      {"api_version", CanonicalValue(std::string(kResolvedParticipantStopScopePolicyApiVersion))},
      {"operation_id", CanonicalValue(stage.operation_id.value())},
      {"operation_revision", safe_unsigned(stage.operation_revision.value())},
      {"participant_id", CanonicalValue(stage.participant_id.value())},
      {"controllable_component_id", CanonicalValue(stage.controllable_component_id.value())},
      {"command_target_id", CanonicalValue(stage.command_target_id.value())},
      {"capability_id", CanonicalValue(stage.capability_id.value())},
      {"control_domain_id", CanonicalValue(stage.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(stage.control_domain_revision.value())},
      {"control_binding_id", CanonicalValue(stage.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(stage.control_binding_revision.value())},
      {"active_control_configuration_id",
       CanonicalValue(stage.active_control_configuration_id.value())},
      {"active_control_configuration_generation",
       safe_unsigned(stage.active_control_configuration_generation.value())},
      {"controller_topology_id", CanonicalValue(stage.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(stage.topology_revision.value())},
      {"control_route_id", CanonicalValue(stage.control_route_id.value())},
      {"controller_goal_id", CanonicalValue(stage.controller_goal_id.value())},
      {"controller_goal_generation", safe_unsigned(stage.controller_goal_generation.value())},
      {"provider_runtime_epoch", CanonicalValue(stage.provider_runtime_epoch.value())},
      {"resources", CanonicalValue(std::move(resources))},
      {"containment_granularity",
       CanonicalValue(std::string(to_string(stage.containment_granularity)))},
      {"stop_evidence_policy_id", CanonicalValue(stage.stop_evidence_policy_id.value())},
      {"stop_evidence_policy_revision",
       CanonicalValue(stage.stop_evidence_policy_revision.value())},
      {"resolved_stop_base_plan_id",
       CanonicalValue(stage.participant_stop_plan.resolved_stop_base_plan_id.value())},
      {"participant_stop_invocation_plan_digest",
       CanonicalValue(stage.participant_stop_plan.participant_stop_invocation_plan_digest.value())},
      {"stage_index", safe_unsigned(stage.stage_index)},
      {"stop_escalation_stage_id",
       CanonicalValue(command->resolved_stage.stop_escalation_stage_id.value())},
      {"stage_kind", CanonicalValue(std::string(to_string(command->resolved_stage.stage_kind)))},
      {"containment_command",
       containment_command_value(command->resolved_stage.containment_command)}};
  if (stage.capability_provider_binding_id.has_value()) {
    fields.emplace_back("capability_provider_binding_id",
                        CanonicalValue(stage.capability_provider_binding_id->value()));
    fields.emplace_back("capability_provider_binding_revision",
                        CanonicalValue(stage.capability_provider_binding_revision->value()));
  }
  return CanonicalValue(std::move(fields));
}

[[nodiscard]] CanonicalValue evidence_value(const JsonValueDTO& value, std::size_t depth,
                                            std::size_t& nodes) {
  if (depth > kMaximumStopEvidenceValueDepth || ++nodes > kMaximumStopEvidenceValueNodes) {
    throw std::invalid_argument("typed evidence exceeds its structural bound");
  }
  return std::visit(
      [&](const auto& item) -> CanonicalValue {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<Item, bool> || std::is_same_v<Item, std::string>) {
          return CanonicalValue(item);
        } else if constexpr (std::is_same_v<Item, double>) {
          if (!std::isfinite(item)) {
            throw std::invalid_argument("typed evidence is not finite");
          }
          return CanonicalValue(item);
        } else if constexpr (std::is_same_v<Item, JsonValueDTO::Array>) {
          CanonicalValue::Array values;
          values.reserve(item.size());
          for (const auto& child : item) {
            values.emplace_back(evidence_value(child, depth + 1U, nodes));
          }
          return CanonicalValue(std::move(values));
        } else {
          CanonicalValue::Object values;
          values.reserve(item.size());
          for (const auto& [name, child] : item) {
            values.emplace_back(name, evidence_value(child, depth + 1U, nodes));
          }
          return CanonicalValue(std::move(values));
        }
      },
      value.value);
}

[[nodiscard]] bool valid_typed_evidence(const JsonValueDTO& value) noexcept {
  try {
    std::size_t nodes = 0U;
    const auto canonical = evidence_value(value, 1U, nodes);
    return content_identity::canonical_json(canonical).size() <= kMaximumStopEvidenceValueBytes;
  } catch (const std::exception&) {
    return false;
  }
}

[[nodiscard]] std::optional<MotionControlledStopMode> requested_mode(
    const PreparedStopStageCommandDTO& command) noexcept {
  if (!command.validated_contract_parameters.has_value()) {
    return std::nullopt;
  }
  const auto found = command.validated_contract_parameters->find("stop_mode");
  if (found == command.validated_contract_parameters->end()) {
    return std::nullopt;
  }
  const auto* text = std::get_if<std::string>(&found->second.value);
  if (text == nullptr) {
    return std::nullopt;
  }
  if (*text == "soft_on_path") {
    return MotionControlledStopMode::kSoftOnPath;
  }
  if (*text == "spring_back_hold") {
    return MotionControlledStopMode::kSpringBackHold;
  }
  if (*text == "spring_back_fast") {
    return MotionControlledStopMode::kSpringBackFast;
  }
  if (*text == "hard_deceleration") {
    return MotionControlledStopMode::kHardDeceleration;
  }
  return std::nullopt;
}

[[nodiscard]] bool canonical_resources(std::span<const ResourceId> resources) noexcept {
  return !resources.empty() && resources.size() <= kMaximumStopSelectorTargets &&
         std::is_sorted(resources.begin(), resources.end()) &&
         std::adjacent_find(resources.begin(), resources.end()) == resources.end() &&
         std::all_of(resources.begin(), resources.end(),
                     [](const auto& resource) { return identity(resource); });
}

[[nodiscard]] Result<void> validate_stage_shape(const ResolvedParticipantStopStageDTO& stage) {
  if (!identity(stage.stop_id) || !counter(stage.stop_generation) ||
      !identity(stage.operation_id) || !counter(stage.operation_revision) ||
      !identity(stage.participant_id) || !identity(stage.controller_goal_id) ||
      !counter(stage.controller_goal_generation) || !identity(stage.controllable_component_id) ||
      !identity(stage.command_target_id) || !identity(stage.capability_id) ||
      !identity(stage.control_domain_id) || !identity(stage.control_domain_revision) ||
      !identity(stage.control_binding_id) || !identity(stage.control_binding_revision) ||
      !identity(stage.active_control_configuration_id) ||
      !counter(stage.active_control_configuration_generation) ||
      !identity(stage.controller_topology_id) || !identity(stage.topology_revision) ||
      !identity(stage.control_route_id) || !identity(stage.provider_runtime_epoch) ||
      !identity(stage.stop_evidence_policy_id) || !identity(stage.stop_evidence_policy_revision) ||
      !known(stage.containment_granularity) || !canonical_resources(stage.resources) ||
      !manyforge::content_identity::is_sha256_digest(
          stage.resolved_stop_scope_policy_digest.value())) {
    return invalid("participant_stop_stage_identity_invalid");
  }
  if (stage.capability_provider_binding_id.has_value() !=
          stage.capability_provider_binding_revision.has_value() ||
      (stage.capability_provider_binding_id.has_value() &&
       (!identity(*stage.capability_provider_binding_id) ||
        !identity(*stage.capability_provider_binding_revision)))) {
    return invalid("participant_stop_stage_provider_binding_invalid");
  }
  const auto& plan = stage.participant_stop_plan;
  const auto* command = selected_command(stage);
  if (command == nullptr ||
      !manyforge::content_identity::is_sha256_digest(
          plan.participant_stop_invocation_plan_digest.value()) ||
      !identity(plan.resolved_stop_base_plan_id) || plan.operation_id != stage.operation_id ||
      plan.operation_revision != stage.operation_revision ||
      plan.participant_id != stage.participant_id) {
    return invalid("participant_stop_plan_join_invalid");
  }
  const auto& resolved = command->resolved_stage;
  if (resolved.stage_index != stage.stage_index ||
      resolved.containment_granularity != stage.containment_granularity ||
      resolved.command_target.provider_runtime_epoch != stage.provider_runtime_epoch ||
      resolved.evidence_requirements.empty() ||
      resolved.evidence_requirements.size() > kMaximumStopAcceptedPhases) {
    return invalid("participant_stop_stage_selection_invalid");
  }
  return Result<void>::Ok();
}

[[nodiscard]] Result<void> validate_stage_referents(
    const ResolvedParticipantStopStageDTO& stage, const ResolvedStopBasePlanDTO& verified_base_plan,
    const RuntimeParticipantBinding& verified_participant,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category,
    std::span<const ResourceId> expanded_scope_resources) {
  auto shape = validate_stage_shape(stage);
  if (!shape.ok()) {
    return shape;
  }
  if (!canonical_resources(expanded_scope_resources) ||
      !std::equal(stage.resources.begin(), stage.resources.end(), expanded_scope_resources.begin(),
                  expanded_scope_resources.end())) {
    return invalid("participant_stop_stage_expanded_scope_invalid");
  }
  if (!VerifyResolvedStopBasePlan(verified_base_plan).ok()) {
    return invalid("participant_stop_base_plan_unverified");
  }
  const auto& binding = verified_participant.value();
  if (!binding.control_path.has_value()) {
    return invalid("participant_stop_control_path_missing");
  }
  const auto& path = *binding.control_path;
  const auto& plan = stage.participant_stop_plan;
  const auto* command = selected_command(stage);
  if (command == nullptr ||
      !VerifyPreparedParticipantStopPlan(verified_base_plan, plan, binding.resources,
                                         retained_default_limits, node_category)
           .ok()) {
    return invalid("participant_stop_plan_unverified");
  }
  if (plan.program_id != binding.program_id || plan.program_revision != binding.program_revision ||
      plan.program_binding_id != binding.program_binding_id ||
      plan.resolved_stop_base_plan_id != verified_base_plan.resolved_stop_base_plan_id ||
      binding.retained_bundle_id != verified_base_plan.retained_bundle_id ||
      binding.graph_revision != verified_base_plan.graph_revision ||
      stage.controllable_component_id != binding.controllable_component_id ||
      stage.command_target_id != binding.command_target_id ||
      stage.capability_id != binding.capability_id ||
      stage.control_domain_id != path.control_domain_id ||
      stage.control_domain_revision != path.control_domain_revision ||
      stage.control_binding_id != path.control_binding_id ||
      stage.control_binding_revision != path.control_binding_revision ||
      stage.active_control_configuration_id != path.active_control_configuration_id ||
      stage.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      stage.controller_topology_id != path.controller_topology_id ||
      stage.topology_revision != path.topology_revision ||
      stage.control_route_id != path.control_route_id ||
      stage.stop_evidence_policy_id != path.stop_evidence_policy_id ||
      stage.stop_evidence_policy_revision != path.stop_evidence_policy_revision ||
      verified_base_plan.control_domain_id != path.control_domain_id ||
      verified_base_plan.control_domain_revision != path.control_domain_revision ||
      verified_base_plan.control_binding_id != path.control_binding_id ||
      verified_base_plan.control_binding_revision != path.control_binding_revision ||
      verified_base_plan.controller_topology_id != path.controller_topology_id ||
      verified_base_plan.topology_revision != path.topology_revision ||
      verified_base_plan.control_route_id != path.control_route_id ||
      verified_base_plan.activation_set_id != path.activation_set_id ||
      verified_base_plan.active_control_configuration_id != path.active_control_configuration_id ||
      verified_base_plan.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      verified_base_plan.stop_evidence_policy_id != path.stop_evidence_policy_id ||
      verified_base_plan.stop_evidence_policy_revision != path.stop_evidence_policy_revision) {
    return invalid("participant_stop_runtime_join_invalid");
  }
  const auto& resolved = command->resolved_stage;
  const RuntimeCapabilityProviderPathDTO* selected_capability_provider = nullptr;
  const auto matches_binding_pair = [&](const RuntimeCapabilityProviderPathDTO& candidate) {
    return stage.capability_provider_binding_id.has_value() &&
           candidate.capability_provider_binding_id == *stage.capability_provider_binding_id &&
           candidate.capability_provider_binding_revision ==
               *stage.capability_provider_binding_revision;
  };
  if (binding.capability_provider_path.has_value() &&
      matches_binding_pair(*binding.capability_provider_path)) {
    selected_capability_provider = &*binding.capability_provider_path;
  }
  const auto supporting_provider =
      std::find_if(binding.supporting_capability_provider_paths.begin(),
                   binding.supporting_capability_provider_paths.end(), matches_binding_pair);
  if (selected_capability_provider == nullptr &&
      supporting_provider != binding.supporting_capability_provider_paths.end()) {
    selected_capability_provider = &*supporting_provider;
  }
  const auto target_matches_capability_provider =
      [&](const RuntimeCapabilityProviderPathDTO& candidate) {
        return target_matches_provider(resolved.command_target, candidate);
      };
  const bool capability_provider_target_available =
      (binding.capability_provider_path.has_value() &&
       target_matches_capability_provider(*binding.capability_provider_path)) ||
      std::any_of(binding.supporting_capability_provider_paths.begin(),
                  binding.supporting_capability_provider_paths.end(),
                  target_matches_capability_provider);
  const bool control_path_target_available =
      target_matches_provider(resolved.command_target, path);
  if ((stage.capability_provider_binding_id.has_value() &&
       selected_capability_provider == nullptr) ||
      (!stage.capability_provider_binding_id.has_value() &&
       capability_provider_target_available && !control_path_target_available)) {
    return invalid("participant_stop_capability_provider_join_invalid");
  }
  const bool target_matches_selected_provider =
      selected_capability_provider != nullptr
          ? target_matches_provider(resolved.command_target, *selected_capability_provider)
          : target_matches_provider(resolved.command_target, path);
  const bool target_epoch_join =
      std::count_if(binding.runtime_unit_epochs.begin(), binding.runtime_unit_epochs.end(),
                    [&](const RuntimeUnitEpochBindingDTO& row) {
                      return row.runtime_unit_id == resolved.command_target.runtime_unit_id &&
                             row.runtime_epoch == resolved.command_target.runtime_unit_epoch;
                    }) == 1;
  if (!target_matches_selected_provider || !target_epoch_join ||
      containment_rank(stage.containment_granularity) <
          containment_rank(path.containment_granularity)) {
    return invalid("participant_stop_stage_target_or_containment_join_invalid");
  }
  const auto scope_activation_matches = [&](const auto& scope) {
    return scope.activation_set_id == path.activation_set_id &&
           scope.activation_set_id == verified_base_plan.activation_set_id;
  };
  switch (stage.containment_granularity) {
    case ContainmentGranularity::kResource: {
      const auto* scope =
          std::get_if<ResolvedStopResourceContainmentScopeDTO>(&resolved.containment_scope);
      if (scope == nullptr || !scope_activation_matches(*scope) ||
          scope->resource_ids != stage.resources) {
        return invalid("participant_stop_stage_scope_invalid");
      }
      break;
    }
    case ContainmentGranularity::kControllerSet: {
      const auto* scope =
          std::get_if<ResolvedStopControllerSetContainmentScopeDTO>(&resolved.containment_scope);
      if (scope == nullptr || !scope_activation_matches(*scope)) {
        return invalid("participant_stop_stage_scope_invalid");
      }
      break;
    }
    case ContainmentGranularity::kControlDomain: {
      const auto* scope =
          std::get_if<ResolvedStopControlDomainContainmentScopeDTO>(&resolved.containment_scope);
      if (scope == nullptr || !scope_activation_matches(*scope) ||
          scope->control_domain_id != stage.control_domain_id ||
          scope->control_domain_revision != stage.control_domain_revision) {
        return invalid("participant_stop_stage_scope_invalid");
      }
      break;
    }
  }
  for (const auto& resource : binding.resources) {
    if (!std::binary_search(stage.resources.begin(), stage.resources.end(), resource.resource_id)) {
      return invalid("participant_stop_stage_scope_omits_participant_resource");
    }
  }
  const auto digest = ComputeResolvedParticipantStopScopePolicyDigest(stage);
  if (!digest.ok() || digest.value() != stage.resolved_stop_scope_policy_digest) {
    return invalid("participant_stop_scope_policy_digest_mismatch");
  }
  return Result<void>::Ok();
}

[[nodiscard]] Result<void> validate_budget(const ResolvedParticipantStopStageDTO& stage,
                                           const RemainingStopBudgetDTO& budget) {
  // Only this robot's own Stop-time values; a controlled deadline only
  // for a controlled-Stop command (hard stages are bounded by the lane;
  // gripper cancels are fire-and-forget).
  const auto* command = selected_command(stage);
  if (command == nullptr || budget.stop_id != stage.stop_id ||
      budget.stop_generation != stage.stop_generation || budget.elapsed_since_stop_ns < 0 ||
      budget.elapsed_since_stop_ns > kMaximumStopDurationNs ||
      (budget.stop_ingress_age_ns &&
       (*budget.stop_ingress_age_ns < 0 ||
        *budget.stop_ingress_age_ns > kMaximumStopDurationNs)) ||
      (budget.remaining_controlled_deadline_ns.has_value() &&
       (command->resolved_stage.containment_command.command_kind !=
            StopContainmentCommandKind::kMotionControlledStop ||
        *budget.remaining_controlled_deadline_ns < 0 ||
        *budget.remaining_controlled_deadline_ns > kMaximumStopDurationNs))) {
    return invalid("participant_stop_remaining_budget_invalid");
  }
  return Result<void>::Ok();
}

}  // namespace

Result<ContentDigest> ComputeResolvedParticipantStopScopePolicyDigest(
    const ResolvedParticipantStopStageDTO& stage) {
  try {
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        kResolvedParticipantStopScopePolicyIdentityDomain,
        kResolvedParticipantStopScopePolicyApiVersion, digest_projection(stage))));
  } catch (const std::exception&) {
    return invalid_value<ContentDigest>("participant_stop_scope_policy_projection_invalid");
  }
}

Result<ResolvedParticipantStopStageDTO> MakeResolvedParticipantStopStage(
    ResolvedParticipantStopStageDTO stage, const RemainingStopBudgetDTO& budget,
    const ResolvedStopBasePlanDTO& verified_base_plan,
    const RuntimeParticipantBinding& verified_participant,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category,
    std::span<const ResourceId> expanded_scope_resources) {
  auto digest = ComputeResolvedParticipantStopScopePolicyDigest(stage);
  if (!digest.ok()) {
    return Result<ResolvedParticipantStopStageDTO>::Err(digest.error());
  }
  stage.resolved_stop_scope_policy_digest = std::move(digest.value());
  auto valid = ValidateResolvedParticipantStopStage(stage, budget, verified_base_plan,
                                                    verified_participant, retained_default_limits,
                                                    node_category, expanded_scope_resources);
  if (!valid.ok()) {
    return Result<ResolvedParticipantStopStageDTO>::Err(valid.error());
  }
  return Result<ResolvedParticipantStopStageDTO>::Ok(std::move(stage));
}

Result<void> ValidateResolvedParticipantStopStage(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    const ResolvedStopBasePlanDTO& verified_base_plan,
    const RuntimeParticipantBinding& verified_participant,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category,
    std::span<const ResourceId> expanded_scope_resources) {
  auto stage_valid =
      validate_stage_referents(stage, verified_base_plan, verified_participant,
                               retained_default_limits, node_category, expanded_scope_resources);
  if (!stage_valid.ok()) {
    return stage_valid;
  }
  return validate_budget(stage, budget);
}

Result<void> ValidateParticipantStopStageResult(
    const ParticipantStopStageResultDTO& result, const ResolvedStopBasePlanDTO& verified_base_plan,
    const RuntimeParticipantBinding& verified_participant,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category,
    std::span<const ResourceId> expanded_scope_resources) {
  auto shape =
      validate_stage_referents(result.request, verified_base_plan, verified_participant,
                               retained_default_limits, node_category, expanded_scope_resources);
  if (!shape.ok()) {
    return shape;
  }
  if (!known(result.attempt_disposition) || !known(result.outcome) ||
      result.receiver_elapsed_duration_ns < 0 ||
      result.receiver_elapsed_duration_ns > kMaximumStopDurationNs ||
      (result.recoverable_substitution_reason.has_value() &&
       !motion_interrupt_stop_profile_id_valid(*result.recoverable_substitution_reason)) ||
      result.source_epoch.has_value() != result.source_sequence.has_value() ||
      (result.source_epoch.has_value() &&
       (!identity(*result.source_epoch) || !safe_sequence(*result.source_sequence))) ||
      (result.controller_acknowledgement_sequence.has_value() &&
       !safe_sequence(*result.controller_acknowledgement_sequence)) ||
      (result.error.has_value() && !stable_error(*result.error))) {
    return invalid("participant_stop_stage_result_shape_invalid");
  }
  const auto* command = selected_command(result.request);
  if (command == nullptr ||
      result.evidence_items.size() != command->resolved_stage.evidence_requirements.size()) {
    return invalid("participant_stop_stage_result_evidence_invalid");
  }
  if (result.source_epoch.has_value() &&
      *result.source_epoch != command->resolved_stage.command_target.provider_runtime_epoch) {
    return invalid("participant_stop_stage_result_source_epoch_invalid");
  }
  const auto expected_mode = requested_mode(*command);
  if (!expected_mode.has_value() &&
      (result.actual_provider_contract_mode.has_value() ||
       result.recoverable_substitution_reason.has_value())) {
    return invalid("participant_stop_stage_result_mode_not_defined");
  }
  if (result.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched &&
      (result.actual_provider_contract_mode.has_value() ||
       result.recoverable_substitution_reason.has_value() ||
       result.controller_acknowledgement_sequence.has_value() || result.source_epoch.has_value())) {
    return invalid("participant_stop_stage_not_dispatched_evidence_invalid");
  }
  if (result.actual_provider_contract_mode.has_value()) {
    if (!known(*result.actual_provider_contract_mode)) {
      return invalid("participant_stop_stage_result_mode_invalid");
    }
    if (*result.actual_provider_contract_mode == *expected_mode) {
      if (result.recoverable_substitution_reason.has_value()) {
        return invalid("participant_stop_stage_unneeded_substitution_provenance");
      }
    } else {
      const auto& contract =
          command->resolved_stage.command_qualification
              .controlled_substitution_contract;
      const auto& profile = result.request.participant_stop_plan.participant_stop_profile;
      const auto actual = *result.actual_provider_contract_mode;
      if (!result.recoverable_substitution_reason.has_value() || !contract.has_value() ||
          !profile.has_value() ||
          std::find(contract->fallback_stop_modes.begin(), contract->fallback_stop_modes.end(),
                    actual) == contract->fallback_stop_modes.end() ||
          std::none_of(profile->fallback_stop_modes.begin(), profile->fallback_stop_modes.end(),
                       [&](const auto mode) {
                         return (mode == MotionInterruptStopMode::kSpringBackHold &&
                                 actual == MotionControlledStopMode::kSpringBackHold) ||
                                (mode == MotionInterruptStopMode::kSpringBackFast &&
                                 actual == MotionControlledStopMode::kSpringBackFast);
                       })) {
        return invalid("participant_stop_stage_substitution_unqualified");
      }
    }
  }
  if (expected_mode.has_value() &&
      result.attempt_disposition != StopStageAttemptDispositionDTO::kNotDispatched &&
      !result.actual_provider_contract_mode.has_value()) {
    return invalid("participant_stop_stage_actual_mode_missing");
  }

  bool has_satisfied = false;
  bool has_unsatisfied = false;
  for (std::size_t index = 0U; index < result.evidence_items.size(); ++index) {
    const auto& actual = result.evidence_items[index];
    const auto& expected = command->resolved_stage.evidence_requirements[index];
    const bool live = actual.source_epoch.has_value();
    if (actual.evidence_item_id != expected.requirement.evidence_item_id ||
        actual.required_class != expected.requirement.evidence_kind ||
        actual.resolved_source != expected.resolved_source || !known(actual.state) ||
        live != actual.source_sequence.has_value() ||
        live != actual.source_age_at_evaluation_ns.has_value() ||
        (live &&
         (!identity(*actual.source_epoch) ||
          *actual.source_epoch != expected.resolved_source.runtime_target.provider_runtime_epoch ||
          !safe_sequence(*actual.source_sequence) || *actual.source_age_at_evaluation_ns < 0 ||
          *actual.source_age_at_evaluation_ns > kMaximumStopDurationNs)) ||
        (actual.evidence.has_value() && !valid_typed_evidence(*actual.evidence)) ||
        (actual.error.has_value() && !stable_error(*actual.error))) {
      return invalid("participant_stop_stage_result_evidence_invalid");
    }
    switch (actual.state) {
      case StopEvidenceItemStateDTO::kRequiredPending:
        return invalid("participant_stop_stage_result_evidence_pending");
      case StopEvidenceItemStateDTO::kSatisfied:
        if (!live || !actual.evidence.has_value() || actual.error.has_value() ||
            *actual.source_age_at_evaluation_ns >
                expected.requirement.source_requirement.freshness_budget_ns) {
          return invalid("participant_stop_stage_satisfied_evidence_invalid");
        }
        has_satisfied = true;
        break;
      case StopEvidenceItemStateDTO::kMissing:
        if (actual.evidence.has_value() || !actual.error.has_value()) {
          return invalid("participant_stop_stage_missing_evidence_invalid");
        }
        has_unsatisfied = true;
        break;
      case StopEvidenceItemStateDTO::kFailed:
        if (!actual.error.has_value()) {
          return invalid("participant_stop_stage_failed_evidence_invalid");
        }
        has_unsatisfied = true;
        break;
      case StopEvidenceItemStateDTO::kInapplicable:
        if (live || actual.evidence.has_value() || !actual.error.has_value() ||
            actual.error->code != "stop_stage_superseded_before_dispatch") {
          return invalid("participant_stop_stage_inapplicable_evidence_invalid");
        }
        has_unsatisfied = true;
        break;
    }
  }
  if (result.outcome == ParticipantStopStageOutcomeDTO::kSupersededNoEffect) {
    if (result.attempt_disposition != StopStageAttemptDispositionDTO::kNotDispatched ||
        result.actual_provider_contract_mode.has_value() ||
        result.recoverable_substitution_reason.has_value() ||
        result.controller_acknowledgement_sequence.has_value() || result.source_epoch.has_value() ||
        !result.error.has_value() ||
        result.error->code != "stop_stage_superseded_before_dispatch" ||
        std::any_of(result.evidence_items.begin(), result.evidence_items.end(),
                    [](const auto& item) {
                      return item.state != StopEvidenceItemStateDTO::kInapplicable;
                    })) {
      return invalid("participant_stop_stage_superseded_result_invalid");
    }
  } else {
    if (std::any_of(result.evidence_items.begin(), result.evidence_items.end(),
                    [](const auto& item) {
                      return item.state == StopEvidenceItemStateDTO::kInapplicable;
                    })) {
      return invalid("participant_stop_stage_ordinary_inapplicable_evidence");
    }
    if (result.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched &&
        result.outcome != ParticipantStopStageOutcomeDTO::kFailed) {
      return invalid("participant_stop_stage_not_dispatched_result_invalid");
    }
    if (result.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched &&
        std::any_of(result.evidence_items.begin(), result.evidence_items.end(),
                    [](const auto& item) {
                      return item.source_epoch.has_value() || item.evidence.has_value() ||
                             (item.state != StopEvidenceItemStateDTO::kMissing &&
                              item.state != StopEvidenceItemStateDTO::kFailed);
                    })) {
      return invalid("participant_stop_stage_not_dispatched_live_evidence");
    }
    if (result.outcome == ParticipantStopStageOutcomeDTO::kSucceeded &&
        (result.attempt_disposition == StopStageAttemptDispositionDTO::kNotDispatched ||
         result.error.has_value() || has_unsatisfied || !has_satisfied)) {
      return invalid("participant_stop_stage_success_result_invalid");
    }
    if (result.outcome == ParticipantStopStageOutcomeDTO::kPartial &&
        (!result.error.has_value() || !has_satisfied || !has_unsatisfied)) {
      return invalid("participant_stop_stage_partial_result_invalid");
    }
    if (result.outcome == ParticipantStopStageOutcomeDTO::kFailed && !result.error.has_value()) {
      return invalid("participant_stop_stage_failed_result_invalid");
    }
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
