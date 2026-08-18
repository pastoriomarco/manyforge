#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_roboplan/retained_stop_surveillance_owner.hpp>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using manyforge::content_identity::CanonicalValue;

inline constexpr std::string_view kResourceEvidenceDomain =
    "stop-fault-recovery-resource-evidence-digest";
inline constexpr std::string_view kResourceEvidenceApiVersion =
    "manyforge.stop-fault-recovery-resource-evidence.v1";
inline constexpr std::string_view kConfigurationEvidenceDomain =
    "stop-fault-recovery-configuration-evidence-digest";
inline constexpr std::string_view kConfigurationEvidenceApiVersion =
    "manyforge.stop-fault-recovery-configuration-evidence.v1";
inline constexpr std::string_view kCurrentResourceEvidenceDomain =
    "native-current-resource-evidence-digest";
inline constexpr std::string_view kCurrentResourceEvidenceApiVersion =
    "manyforge.native-current-resource-evidence.v1";
inline constexpr std::string_view kExternalReconciliationEvidenceDomain =
    "stop-fault-external-reconciliation-evidence-digest";
inline constexpr std::string_view kExternalReconciliationEvidenceApiVersion =
    "manyforge.stop-fault-external-reconciliation-evidence.v1";

[[nodiscard]] core::ErrorDTO error(std::string code, std::string message) {
  return core::ErrorDTO{std::move(code), std::move(message), {}};
}

template <typename Value>
[[nodiscard]] core::Result<Value> invalid(std::string code,
                                          std::string message) {
  return core::Result<Value>::Err(error(std::move(code), std::move(message)));
}

[[nodiscard]] std::vector<core::ResourceId>
binding_resources(const core::RuntimeParticipantBinding &binding) {
  std::vector<core::ResourceId> result;
  result.reserve(binding.value().resources.size());
  for (const auto &resource : binding.value().resources) {
    result.push_back(resource.resource_id);
  }
  std::sort(result.begin(), result.end());
  return result;
}

[[nodiscard]] bool source_matches_binding(
    const core::ResolvedStopEvidenceSourceDTO &source,
    const core::RuntimeParticipantBinding &binding) noexcept {
  const auto &value = binding.value();
  if (!value.control_path.has_value() ||
      source.source_kind !=
          core::StopEvidenceSourceKind::kBindingStateAuthority ||
      source.state_authority_id !=
          std::optional(value.control_path->state_authority_id) ||
      source.state_authority_class !=
          std::optional(value.control_path->state_authority_class)) {
    return false;
  }
  const auto &path = *value.control_path;
  const auto &target = source.runtime_target;
  const auto runtime_epoch =
      std::find_if(value.runtime_unit_epochs.begin(),
                   value.runtime_unit_epochs.end(), [&](const auto &row) {
                     return row.runtime_unit_id == target.runtime_unit_id;
                   });
  return target.provider_instance_id == path.provider_instance_id &&
         target.provider_manifest_id == path.provider_manifest_id &&
         target.provider_manifest_revision == path.provider_manifest_revision &&
         target.runtime_unit_id == path.provider_runtime_unit_id &&
         target.provider_runtime_epoch == path.provider_runtime_epoch &&
         runtime_epoch != value.runtime_unit_epochs.end() &&
         target.runtime_unit_epoch == runtime_epoch->runtime_epoch;
}

[[nodiscard]] bool
source_input_valid(const RetainedStopSurveillanceSourceInput &source) {
  if (!source.state_channel ||
      !source.binding.value().control_path.has_value() ||
      (source.observation_sources.empty() &&
       !source.no_active_goal_evidence_source))
    return false;
  const auto &context = source.state_channel->query_context();
  auto expected = core::make_observed_state_query_context(
      source.binding, context.freshness_budget_ns);
  if (!expected.ok() || expected.value() != context)
    return false;
  const auto resources = binding_resources(source.binding);
  for (const auto &observation : source.observation_sources) {
    if (observation.evidence_requirement.evidence_kind !=
            core::StopEvidenceRequirementKind::kMotionRest ||
        observation.evidence_requirement.source_requirement.source_kind !=
            core::StopEvidenceSourceKind::kBindingStateAuthority ||
        observation.evidence_requirement.source_requirement
                .freshness_budget_ns <= 0 ||
        observation.evidence_requirement.observation_semantics
                .observation_kind ==
            core::StopEvidenceObservationKind::kCommandResult ||
        !observation.resolved_source.state_authority_class.has_value() ||
        std::find(observation.evidence_requirement.source_requirement
                      .allowed_authority_classes.begin(),
                  observation.evidence_requirement.source_requirement
                      .allowed_authority_classes.end(),
                  *observation.resolved_source.state_authority_class) ==
            observation.evidence_requirement.source_requirement
                .allowed_authority_classes.end() ||
        !source_matches_binding(observation.resolved_source, source.binding) ||
        observation.observed_resources.empty() ||
        !std::is_sorted(observation.observed_resources.begin(),
                        observation.observed_resources.end()) ||
        std::adjacent_find(observation.observed_resources.begin(),
                           observation.observed_resources.end()) !=
            observation.observed_resources.end() ||
        !std::includes(resources.begin(), resources.end(),
                       observation.observed_resources.begin(),
                       observation.observed_resources.end())) {
      return false;
    }
  }
  for (std::size_t left = 0U; left < source.observation_sources.size();
       ++left) {
    for (std::size_t right = left + 1U;
         right < source.observation_sources.size(); ++right) {
      const auto &first = source.observation_sources[left];
      const auto &second = source.observation_sources[right];
      if (first.evidence_requirement == second.evidence_requirement &&
          first.resolved_source == second.resolved_source) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] core::StopSurveillanceObservationSampleDTO unavailable_sample(
    const core::StopSurveillanceObservationQueryDTO &query,
    const core::StopSurveillanceSourceAvailabilityDTO availability,
    std::string code, std::string message) {
  return core::StopSurveillanceObservationSampleDTO{
      query.stop_certificate,
      query.scope_key,
      query.evidence_requirement.evidence_item_id,
      query.resolved_source,
      availability,
      std::nullopt,
      error(std::move(code), std::move(message))};
}

[[nodiscard]] double
velocity_value(const core::CoordinateVelocityDTO &velocity) noexcept {
  return std::visit(
      [](const auto &value) {
        if constexpr (requires { value.value_rad_s; }) {
          return value.value_rad_s;
        } else {
          return value.value_m_s;
        }
      },
      velocity);
}

[[nodiscard]] core::ContentDigest
resource_evidence_digest(const core::StopSurveillanceObservationQueryDTO &query,
                         const core::StopEvidenceItemDTO &evidence,
                         const core::ResourceId &resource,
                         const core::StopFaultRecoveryResourceStateDTO state) {
  const CanonicalValue value(CanonicalValue::Object{
      {"evidence_item_id", CanonicalValue(evidence.evidence_item_id.value())},
      {"resource_id", CanonicalValue(resource.value())},
      {"scope_origin_stop_generation",
       CanonicalValue(
           std::to_string(query.scope_key.origin_stop_generation.value()))},
      {"scope_origin_stop_id",
       CanonicalValue(query.scope_key.origin_stop_id.value())},
      {"source_age_ns",
       CanonicalValue(std::to_string(*evidence.source_age_at_evaluation_ns))},
      {"source_epoch", CanonicalValue(evidence.source_epoch->value())},
      {"source_sequence",
       CanonicalValue(std::to_string(*evidence.source_sequence))},
      {"state",
       CanonicalValue(state == core::StopFaultRecoveryResourceStateDTO::kSettled
                          ? "settled"
                          : "unsettled")},
      {"stop_certificate_id",
       CanonicalValue(query.stop_certificate.stop_certificate_id.value())},
      {"stop_certificate_revision",
       CanonicalValue(std::to_string(
           query.stop_certificate.stop_certificate_revision.value()))},
  });
  return core::ContentDigest(content_identity::framed_sha256(
      kResourceEvidenceDomain, kResourceEvidenceApiVersion, value));
}

[[nodiscard]] core::ContentDigest configuration_evidence_digest(
    const core::StopFaultRecoveryConfigurationExpectationDTO &configuration,
    const core::RetainedBundleId &bundle, const core::GraphRevision &graph) {
  const CanonicalValue value(CanonicalValue::Object{
      {"active_control_configuration_generation",
       CanonicalValue(std::to_string(
           configuration.active_control_configuration_generation.value()))},
      {"active_control_configuration_id",
       CanonicalValue(configuration.active_control_configuration_id.value())},
      {"control_domain_id",
       CanonicalValue(configuration.control_domain_id.value())},
      {"graph_revision", CanonicalValue(graph.value())},
      {"retained_bundle_id", CanonicalValue(bundle.value())},
  });
  return core::ContentDigest(content_identity::framed_sha256(
      kConfigurationEvidenceDomain, kConfigurationEvidenceApiVersion, value));
}

[[nodiscard]] core::ContentDigest current_resource_evidence_digest(
    const core::OrphanResourceReconciliationFactDTO &fact,
    const core::ObservedStateSnapshotDTO &snapshot,
    const core::ContentDigest &no_active_goal_evidence_digest,
    const core::ContentDigest &configuration_digest,
    const std::int64_t assessed_at_ns) {
  const CanonicalValue value(CanonicalValue::Object{
      {"active_control_configuration_generation",
       CanonicalValue(std::to_string(
           fact.active_control_configuration_generation.value()))},
      {"active_control_configuration_id",
       CanonicalValue(fact.active_control_configuration_id.value())},
      {"assessed_at_ns", CanonicalValue(std::to_string(assessed_at_ns))},
      {"configuration_evidence_digest",
       CanonicalValue(configuration_digest.value())},
      {"control_binding_id", CanonicalValue(fact.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(fact.control_binding_revision.value())},
      {"control_domain_id", CanonicalValue(fact.control_domain_id.value())},
      {"no_active_goal_evidence_digest",
       CanonicalValue(no_active_goal_evidence_digest.value())},
      {"provider_runtime_epoch",
       CanonicalValue(fact.provider_runtime_epoch.value())},
      {"provider_runtime_unit_id",
       CanonicalValue(fact.provider_runtime_unit_id.value())},
      {"resource_id", CanonicalValue(fact.resource_id.value())},
      {"source_epoch", CanonicalValue(snapshot.source_epoch.value())},
      {"source_sequence", CanonicalValue(std::to_string(snapshot.sequence))},
  });
  return core::ContentDigest(content_identity::framed_sha256(
      kCurrentResourceEvidenceDomain, kCurrentResourceEvidenceApiVersion,
      value));
}

[[nodiscard]] core::ContentDigest external_reconciliation_evidence_digest(
    const core::StopFaultLatchDTO &latch, const core::ResourceId &resource,
    const core::ContentDigest &settlement_digest,
    const core::ContentDigest &no_active_goal_digest,
    const core::ContentDigest &configuration_digest) {
  const CanonicalValue value(CanonicalValue::Object{
      {"configuration_evidence_digest",
       CanonicalValue(configuration_digest.value())},
      {"latch_generation",
       CanonicalValue(std::to_string(latch.latch_generation.value()))},
      {"latch_id", CanonicalValue(latch.latch_id.value())},
      {"no_active_goal_evidence_digest",
       CanonicalValue(no_active_goal_digest.value())},
      {"resource_id", CanonicalValue(resource.value())},
      {"settlement_evidence_digest", CanonicalValue(settlement_digest.value())},
      {"stop_generation",
       CanonicalValue(std::to_string(latch.stop_generation.value()))},
      {"stop_id", CanonicalValue(latch.stop_id.value())},
  });
  return core::ContentDigest(content_identity::framed_sha256(
      kExternalReconciliationEvidenceDomain,
      kExternalReconciliationEvidenceApiVersion, value));
}

struct SelectedSource final {
  std::shared_ptr<IRetainedObservedStateChannel> channel;
  RetainedStopSurveillanceObservationSource observation;
};

[[nodiscard]] bool snapshot_matches_context(
    const core::ObservedStateSnapshotDTO &snapshot,
    const core::ObservedStateQueryContextDTO &context) noexcept {
  if (!snapshot.complete ||
      snapshot.controllable_component_id != context.controllable_component_id ||
      snapshot.command_target_id != context.command_target_id ||
      snapshot.control_domain_id != context.control_domain_id ||
      snapshot.control_domain_revision != context.control_domain_revision ||
      snapshot.control_binding_id != context.control_binding_id ||
      snapshot.control_binding_revision != context.control_binding_revision ||
      snapshot.provider_instance_id != context.provider_instance_id ||
      snapshot.provider_manifest_id != context.provider_manifest_id ||
      snapshot.provider_manifest_revision !=
          context.provider_manifest_revision ||
      snapshot.controller_topology_id != context.controller_topology_id ||
      snapshot.topology_revision != context.topology_revision ||
      snapshot.control_route_id != context.control_route_id ||
      snapshot.active_control_configuration_id !=
          context.active_control_configuration_id ||
      snapshot.active_control_configuration_generation !=
          context.active_control_configuration_generation ||
      snapshot.resources != context.resources ||
      snapshot.joint_names != context.joint_names ||
      snapshot.positions.size() != context.coordinate_kinds.size() ||
      !snapshot.velocities.has_value() ||
      snapshot.velocities->size() != context.coordinate_kinds.size() ||
      snapshot.source_epoch != context.runtime_epoch ||
      snapshot.state_authority_id != context.state_authority_id ||
      snapshot.state_authority_class != context.state_authority_class) {
    return false;
  }
  for (std::size_t index = 0U; index < context.coordinate_kinds.size();
       ++index) {
    if (core::coordinate_kind(snapshot.positions[index]) !=
            context.coordinate_kinds[index] ||
        core::coordinate_kind((*snapshot.velocities)[index]) !=
            context.coordinate_kinds[index]) {
      return false;
    }
  }
  return true;
}

struct CurrentQuiescenceObservation final {
  core::ObservedStateSnapshotDTO snapshot;
  std::int64_t source_age_ns{};
  core::ContentDigest no_active_goal_evidence_digest;
  core::ContentDigest configuration_evidence_digest;
};

struct RefreshedCurrentQuiescenceObservation final {
  CurrentQuiescenceObservation observation;
  std::int64_t assessed_at_ns{};
};

[[nodiscard]] bool quiescence_refreshable(const core::ErrorDTO &failure) {
  if (failure.code.rfind("native_current_resource_", 0U) == 0U ||
      failure.code.rfind("retained_state_", 0U) == 0U ||
      failure.code.rfind("observed_state_", 0U) == 0U ||
      failure.code == "observation_stale") {
    return true;
  }
  const auto reason = failure.details.find("reason");
  if (reason == failure.details.end()) {
    return failure.code == "invalid_request";
  }
  return reason->second == "observation_stale" ||
         reason->second.rfind("native_current_resource_", 0U) == 0U ||
         reason->second.rfind("retained_state_", 0U) == 0U ||
         reason->second.rfind("observed_state_", 0U) == 0U;
}

[[nodiscard]] core::Result<CurrentQuiescenceObservation>
observe_current_quiescence(const RetainedStopSurveillanceSourceInput &source,
                           const std::vector<core::ResourceId> &resources,
                           const std::int64_t assessed_at_ns,
                           const std::int64_t read_deadline_ns = -1) {
  if (!source.no_active_goal_evidence_source || resources.empty()) {
    return invalid<CurrentQuiescenceObservation>(
        "native_no_active_goal_source_missing",
        "native current-resource proof has no goal-activity source");
  }
  auto observed =
      read_deadline_ns >= 0
          ? source.state_channel->current_state_bounded(
                assessed_at_ns, read_deadline_ns, std::stop_token{})
          : source.state_channel->current_state(assessed_at_ns);
  if (!observed.ok()) {
    return core::Result<CurrentQuiescenceObservation>::Err(observed.error());
  }
  const auto &snapshot = observed.value();
  const auto &context = source.state_channel->query_context();
  if (!core::validate_observed_state_snapshot(snapshot, assessed_at_ns).ok() ||
      !snapshot_matches_context(snapshot, context) ||
      assessed_at_ns < snapshot.received_timestamp_ns) {
    return invalid<CurrentQuiescenceObservation>(
        "native_current_resource_identity_mismatch",
        "native current-resource state differs from its retained source");
  }
  const auto age_ns = assessed_at_ns - snapshot.received_timestamp_ns;
  if (age_ns > kRetainedNativeReconciliationFreshnessBudgetNs) {
    return invalid<CurrentQuiescenceObservation>(
        "native_current_resource_state_stale",
        "native current-resource state is too old for reconciliation");
  }
  for (const auto &resource : resources) {
    const auto found = std::find(snapshot.resources.begin(),
                                 snapshot.resources.end(), resource);
    if (found == snapshot.resources.end() || !snapshot.velocities.has_value()) {
      return invalid<CurrentQuiescenceObservation>(
          "native_current_resource_state_incomplete",
          "native current-resource state omits a requested velocity");
    }
    const auto index =
        static_cast<std::size_t>(found - snapshot.resources.begin());
    if (std::abs(velocity_value((*snapshot.velocities)[index])) > 0.0) {
      return invalid<CurrentQuiescenceObservation>(
          "native_current_resource_not_quiescent",
          "native current-resource state still reports motion");
    }
  }
  auto no_active_goal = source.no_active_goal_evidence_source(assessed_at_ns);
  if (!no_active_goal.ok()) {
    return core::Result<CurrentQuiescenceObservation>::Err(
        no_active_goal.error());
  }
  if (!content_identity::is_sha256_digest(no_active_goal.value().value())) {
    return invalid<CurrentQuiescenceObservation>(
        "native_no_active_goal_evidence_invalid",
        "native goal-activity source returned an invalid digest");
  }
  const auto &value = source.binding.value();
  const auto &path = *value.control_path;
  const core::StopFaultRecoveryConfigurationExpectationDTO configuration{
      path.control_domain_id, path.active_control_configuration_id,
      path.active_control_configuration_generation};
  return core::Result<CurrentQuiescenceObservation>::Ok(
      CurrentQuiescenceObservation{
          snapshot, age_ns, std::move(no_active_goal.value()),
          configuration_evidence_digest(configuration, value.retained_bundle_id,
                                        value.graph_revision)});
}

[[nodiscard]] core::Result<RefreshedCurrentQuiescenceObservation>
observe_current_quiescence_after_refresh(
    const RetainedStopSurveillanceSourceInput &source,
    const std::vector<core::ResourceId> &resources,
    const std::int64_t assessed_at_ns, core::IClock *clock) {
  const auto deadline_for = [](const std::int64_t assessed) {
    return assessed >
                   std::numeric_limits<std::int64_t>::max() -
                       kRetainedNativeReconciliationFreshnessBudgetNs
               ? std::numeric_limits<std::int64_t>::max()
               : assessed + kRetainedNativeReconciliationFreshnessBudgetNs;
  };
  auto assessed = assessed_at_ns;
  auto observed =
      observe_current_quiescence(source, resources, assessed,
                                 deadline_for(assessed));
  if (observed.ok() || !quiescence_refreshable(observed.error())) {
    if (!observed.ok()) {
      return core::Result<RefreshedCurrentQuiescenceObservation>::Err(
          observed.error());
    }
    return core::Result<RefreshedCurrentQuiescenceObservation>::Ok(
        RefreshedCurrentQuiescenceObservation{std::move(observed.value()),
                                               assessed});
  }
  if (!source.state_channel->wait_for_fresh_state(
          std::chrono::nanoseconds(kRetainedNativeReconciliationFreshnessBudgetNs))) {
    return core::Result<RefreshedCurrentQuiescenceObservation>::Err(
        observed.error());
  }
  if (clock != nullptr) {
    assessed = std::max(assessed, clock->now());
  }
  observed = observe_current_quiescence(source, resources, assessed,
                                        deadline_for(assessed));
  if (!observed.ok()) {
    return core::Result<RefreshedCurrentQuiescenceObservation>::Err(
        observed.error());
  }
  return core::Result<RefreshedCurrentQuiescenceObservation>::Ok(
      RefreshedCurrentQuiescenceObservation{std::move(observed.value()),
                                             assessed});
}

[[nodiscard]] std::optional<SelectedSource>
select_source(const std::vector<RetainedStopSurveillanceSourceInput> &sources,
              const core::StopEvidenceRequirementDTO &requirement,
              const core::ResolvedStopEvidenceSourceDTO &resolved_source) {
  std::optional<SelectedSource> result;
  for (const auto &source : sources) {
    for (const auto &observation : source.observation_sources) {
      if (observation.evidence_requirement != requirement ||
          observation.resolved_source != resolved_source) {
        continue;
      }
      if (result.has_value())
        return std::nullopt;
      result = SelectedSource{source.state_channel, observation};
    }
  }
  return result;
}

[[nodiscard]] core::StopSurveillanceObservationSampleDTO
observe_selected(const core::StopSurveillanceObservationQueryDTO &query,
                 const SelectedSource &selected) {
  auto observed = selected.channel->current_state(query.evaluation_time_ns);
  if (!observed.ok()) {
    return unavailable_sample(
        query, core::StopSurveillanceSourceAvailabilityDTO::kUnavailable,
        "stop_surveillance_source_unavailable", observed.error().message);
  }
  const auto &snapshot = observed.value();
  const auto &context = selected.channel->query_context();
  if (!core::validate_observed_state_snapshot(snapshot,
                                              query.evaluation_time_ns)
           .ok() ||
      !snapshot_matches_context(snapshot, context) ||
      snapshot.source_epoch !=
          query.resolved_source.runtime_target.provider_runtime_epoch ||
      snapshot.state_authority_id !=
          *query.resolved_source.state_authority_id ||
      snapshot.state_authority_class !=
          *query.resolved_source.state_authority_class) {
    return unavailable_sample(
        query, core::StopSurveillanceSourceAvailabilityDTO::kInvalid,
        "stop_surveillance_source_identity_mismatch",
        "native surveillance source identity is inconsistent");
  }
  const auto age = query.evaluation_time_ns - snapshot.received_timestamp_ns;
  if (age > query.evidence_requirement.source_requirement.freshness_budget_ns) {
    return unavailable_sample(
        query, core::StopSurveillanceSourceAvailabilityDTO::kUnavailable,
        "stop_surveillance_source_stale",
        "native surveillance source is stale");
  }
  bool at_rest = true;
  for (const auto &resource : selected.observation.observed_resources) {
    const auto found = std::find(snapshot.resources.begin(),
                                 snapshot.resources.end(), resource);
    if (found == snapshot.resources.end()) {
      return unavailable_sample(
          query, core::StopSurveillanceSourceAvailabilityDTO::kInvalid,
          "stop_surveillance_resource_missing",
          "native surveillance source omits a required resource");
    }
    const auto index =
        static_cast<std::size_t>(found - snapshot.resources.begin());
    if (std::abs(velocity_value((*snapshot.velocities)[index])) > 0.0) {
      at_rest = false;
    }
  }
  const auto state = at_rest ? core::StopEvidenceItemStateDTO::kSatisfied
                             : core::StopEvidenceItemStateDTO::kFailed;
  return core::StopSurveillanceObservationSampleDTO{
      query.stop_certificate,
      query.scope_key,
      query.evidence_requirement.evidence_item_id,
      query.resolved_source,
      core::StopSurveillanceSourceAvailabilityDTO::kAvailable,
      core::StopEvidenceItemDTO{
          query.evidence_requirement.evidence_item_id,
          query.evidence_requirement.evidence_kind, state,
          query.resolved_source, snapshot.source_epoch, snapshot.sequence, age,
          at_rest ? std::optional<core::JsonValueDTO>(core::JsonValueDTO{true})
                  : std::nullopt,
          at_rest ? std::nullopt
                  : std::optional<core::ErrorDTO>(error(
                        "stop_surveillance_motion_not_at_rest",
                        "native addressed state reports continued motion"))},
      std::nullopt};
}

using MotionRestRequirement = std::pair<core::StopEvidenceRequirementDTO,
                                        core::ResolvedStopEvidenceSourceDTO>;

[[nodiscard]] std::vector<MotionRestRequirement>
latest_motion_rest_requirements(const core::StopStatusDTO &status,
                                const core::StopScopeEntryKeyDTO &scope_key) {
  const core::StopAttemptedStageResultDTO *selected = nullptr;
  for (const auto &attempt : status.attempted_stage_results) {
    if (attempt.scope_key == scope_key &&
        attempt.authority ==
            core::StopStageResultAuthorityDTO::kAuthoritative &&
        (selected == nullptr ||
         selected->result_sequence < attempt.result_sequence)) {
      selected = &attempt;
    }
  }
  if (selected == nullptr)
    return {};
  std::vector<MotionRestRequirement> result;
  for (const auto &command :
       selected->result.request.participant_stop_plan.stage_commands) {
    if (command.resolved_stage.stage_index !=
        selected->result.request.stage_index)
      continue;
    for (const auto &requirement :
         command.resolved_stage.evidence_requirements) {
      if (requirement.requirement.evidence_kind ==
              core::StopEvidenceRequirementKind::kMotionRest &&
          requirement.resolved_source.source_kind ==
              core::StopEvidenceSourceKind::kBindingStateAuthority) {
        result.emplace_back(requirement.requirement,
                            requirement.resolved_source);
      }
    }
  }
  return result;
}

} // namespace

RetainedStopSurveillanceOwner::RetainedStopSurveillanceOwner(
    std::vector<RetainedStopSurveillanceSourceInput> sources,
    core::IClock *clock)
    : clock_(clock) {
  auto replaced = ReplaceSources(std::move(sources));
  if (!replaced.ok())
    throw std::invalid_argument(replaced.error().message);
}

core::Result<void> RetainedStopSurveillanceOwner::ValidateSources(
    const std::vector<RetainedStopSurveillanceSourceInput> &sources) {
  if (sources.empty() ||
      sources.size() > core::kMaximumOrphanResourceReconciliationFacts) {
    return invalid<void>("native_stop_surveillance_sources_empty_or_oversized",
                         "native Stop source table is empty or oversized");
  }
  std::size_t source_count = 0U;
  std::set<core::ResourceId> physical_resources;
  const auto &first = sources.front().binding.value();
  for (const auto &source : sources) {
    if (source.observation_sources.size() >
        core::kMaximumStopSurveillanceSources - source_count) {
      return invalid<void>(
          "native_stop_surveillance_sources_oversized",
          "native Stop surveillance source table is oversized");
    }
    source_count += source.observation_sources.size();
    const auto &value = source.binding.value();
    if (value.retained_bundle_id != first.retained_bundle_id ||
        value.graph_revision != first.graph_revision) {
      return invalid<void>(
          "native_stop_surveillance_source_graph_mismatch",
          "native Stop sources cross a retained bundle or graph");
    }
    for (const auto &resource : value.resources) {
      if (!physical_resources.insert(resource.resource_id).second) {
        return invalid<void>(
            "native_stop_surveillance_resource_ambiguous",
            "native Stop sources overlap one physical resource");
      }
    }
  }
  if (!std::all_of(sources.begin(), sources.end(), source_input_valid)) {
    return invalid<void>("native_stop_surveillance_sources_invalid",
                         "native Stop surveillance sources are invalid");
  }
  for (std::size_t left = 0U; left < sources.size(); ++left) {
    for (std::size_t right = left + 1U; right < sources.size(); ++right) {
      for (const auto &first : sources[left].observation_sources) {
        for (const auto &second : sources[right].observation_sources) {
          if (first.evidence_requirement == second.evidence_requirement &&
              first.resolved_source == second.resolved_source) {
            return invalid<void>(
                "native_stop_surveillance_sources_ambiguous",
                "native Stop surveillance source is not unique");
          }
        }
      }
    }
  }
  return core::Result<void>::Ok();
}

core::Result<void> RetainedStopSurveillanceOwner::ReplaceSources(
    std::vector<RetainedStopSurveillanceSourceInput> sources) {
  auto valid = ValidateSources(sources);
  if (!valid.ok())
    return valid;
  std::lock_guard lock(mutex_);
  sources_ = std::move(sources);
  return core::Result<void>::Ok();
}

core::StopSurveillanceObservationSampleDTO
RetainedStopSurveillanceOwner::Observe(
    const core::StopSurveillanceObservationQueryDTO &query) noexcept {
  try {
    if (core::stop_surveillance_observation_query_field_error(query)
            .has_value()) {
      return unavailable_sample(
          query, core::StopSurveillanceSourceAvailabilityDTO::kInvalid,
          "stop_surveillance_query_invalid",
          "native surveillance query is invalid");
    }
    std::optional<SelectedSource> selected;
    {
      std::lock_guard lock(mutex_);
      selected = select_source(sources_, query.evidence_requirement,
                               query.resolved_source);
    }
    if (!selected.has_value()) {
      return unavailable_sample(
          query, core::StopSurveillanceSourceAvailabilityDTO::kInvalid,
          "stop_surveillance_source_unqualified",
          "native surveillance source is absent or ambiguous");
    }
    return observe_selected(query, *selected);
  } catch (const std::exception &exception) {
    return unavailable_sample(
        query, core::StopSurveillanceSourceAvailabilityDTO::kUnavailable,
        "stop_surveillance_observer_failed", exception.what());
  } catch (...) {
    return unavailable_sample(
        query, core::StopSurveillanceSourceAvailabilityDTO::kUnavailable,
        "stop_surveillance_observer_failed",
        "native surveillance observation failed");
  }
}

core::Result<core::StopFaultRecoveryFactsDTO>
RetainedStopSurveillanceOwner::BuildRecoveryFacts(
    const core::StopStatusDTO &status,
    const std::int64_t assessed_at_ns) noexcept {
  try {
    if (assessed_at_ns < 0 ||
        core::stop_status_field_error(status).has_value() ||
        !status.stop_certificate.has_value() ||
        !status.fault_latch.has_value()) {
      return invalid<core::StopFaultRecoveryFactsDTO>(
          "native_stop_recovery_status_invalid",
          "native Stop recovery requires exact terminal status and latch");
    }
    const auto &latch = *status.fault_latch;
    std::vector<RetainedStopSurveillanceSourceInput> sources;
    {
      std::lock_guard lock(mutex_);
      sources = sources_;
    }

    std::map<core::ResourceId, core::StopFaultRecoveryResourceFactDTO>
        resource_facts;
    for (const auto &resource : latch.captured_resources) {
      resource_facts.emplace(
          resource,
          core::StopFaultRecoveryResourceFactDTO{
              resource, core::StopFaultRecoveryResourceStateDTO::kUnavailable,
              std::nullopt, std::nullopt, std::nullopt, std::nullopt,
              std::nullopt,
              error("stop_recovery_source_unavailable",
                    "no fresh native state source covers the captured "
                    "resource")});
    }

    for (const auto &scope : status.scope_entries) {
      const auto *participant =
          std::get_if<core::StopParticipantScopeFactsDTO>(&scope.facts);
      if (participant == nullptr)
        continue;
      const auto requirements =
          latest_motion_rest_requirements(status, scope.key);
      if (requirements.empty())
        continue;
      std::set<core::ResourceId> assigned_resources;
      std::set<core::ResourceId> ambiguous_resources;
      for (const auto &requirement : requirements) {
        const core::StopSurveillanceObservationQueryDTO query{
            *status.stop_certificate, scope.key, requirement.first,
            requirement.second, assessed_at_ns};
        const auto selected =
            select_source(sources, requirement.first, requirement.second);
        if (!selected.has_value())
          continue;
        const auto sample = observe_selected(query, *selected);
        for (const auto &resource : selected->observation.observed_resources) {
          if (!std::binary_search(participant->resources.begin(),
                                  participant->resources.end(), resource)) {
            continue;
          }
          auto found = resource_facts.find(resource);
          if (found == resource_facts.end())
            continue;
          if (ambiguous_resources.contains(resource))
            continue;
          if (!assigned_resources.insert(resource).second) {
            ambiguous_resources.insert(resource);
            found->second = core::StopFaultRecoveryResourceFactDTO{
                resource,
                core::StopFaultRecoveryResourceStateDTO::kUnavailable,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                error("stop_recovery_source_ambiguous",
                      "multiple native rest predicates cover one resource")};
            continue;
          }
          if (sample.availability !=
                  core::StopSurveillanceSourceAvailabilityDTO::kAvailable ||
              !sample.evaluated_evidence.has_value()) {
            found->second = core::StopFaultRecoveryResourceFactDTO{
                resource,
                core::StopFaultRecoveryResourceStateDTO::kUnavailable,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                sample.error.has_value()
                    ? sample.error
                    : std::optional<core::ErrorDTO>(
                          error("stop_recovery_source_unavailable",
                                "native Stop recovery source is unavailable"))};
            continue;
          }
          const auto &item = *sample.evaluated_evidence;
          const auto state =
              item.state == core::StopEvidenceItemStateDTO::kSatisfied
                  ? core::StopFaultRecoveryResourceStateDTO::kSettled
                  : core::StopFaultRecoveryResourceStateDTO::kUnsettled;
          found->second = core::StopFaultRecoveryResourceFactDTO{
              resource,
              state,
              item.source_epoch,
              item.source_sequence,
              item.source_age_at_evaluation_ns,
              query.evidence_requirement.source_requirement.freshness_budget_ns,
              resource_evidence_digest(query, item, resource, state),
              std::nullopt};
        }
      }
    }

    std::map<core::ControlDomainId,
             core::StopFaultRecoveryConfigurationExpectationDTO>
        expectations;
    for (const auto &scope : status.scope_entries) {
      const auto *participant =
          std::get_if<core::StopParticipantScopeFactsDTO>(&scope.facts);
      if (participant == nullptr)
        continue;
      const core::StopFaultRecoveryConfigurationExpectationDTO expectation{
          participant->control_domain_id,
          participant->active_control_configuration_id,
          participant->active_control_configuration_generation};
      const auto [found, inserted] =
          expectations.emplace(expectation.control_domain_id, expectation);
      if (!inserted && found->second != expectation) {
        return invalid<core::StopFaultRecoveryFactsDTO>(
            "native_stop_recovery_configuration_conflict",
            "terminal Stop scope contains conflicting configuration facts");
      }
    }
    if (expectations.empty()) {
      for (const auto &source : sources) {
        const auto &path = *source.binding.value().control_path;
        expectations.emplace(path.control_domain_id,
                             core::StopFaultRecoveryConfigurationExpectationDTO{
                                 path.control_domain_id,
                                 path.active_control_configuration_id,
                                 path.active_control_configuration_generation});
      }
    }

    std::vector<core::StopFaultRecoveryConfigurationFactDTO>
        configuration_facts;
    configuration_facts.reserve(expectations.size());
    for (const auto &[domain, expectation] : expectations) {
      const bool reconciled =
          std::any_of(sources.begin(), sources.end(), [&](const auto &source) {
            const auto &value = source.binding.value();
            return value.retained_bundle_id == latch.retained_bundle_id &&
                   value.graph_revision == latch.graph_revision &&
                   value.control_path.has_value() &&
                   value.control_path->control_domain_id == domain &&
                   value.control_path->active_control_configuration_id ==
                       expectation.active_control_configuration_id &&
                   value.control_path
                           ->active_control_configuration_generation ==
                       expectation.active_control_configuration_generation;
          });
      configuration_facts.push_back(core::StopFaultRecoveryConfigurationFactDTO{
          expectation, reconciled,
          reconciled ? std::optional(configuration_evidence_digest(
                           expectation, latch.retained_bundle_id,
                           latch.graph_revision))
                     : std::nullopt});
    }

    std::vector<core::StopFaultRecoveryResourceFactDTO> external_facts;
    if (latch.trigger == core::StopFaultTriggerDTO::kFailedUncontained) {
      for (const auto &source : sources) {
        std::vector<core::ResourceId> selected_resources;
        for (const auto &resource : binding_resources(source.binding)) {
          if (std::binary_search(latch.captured_resources.begin(),
                                 latch.captured_resources.end(), resource)) {
            selected_resources.push_back(resource);
          }
        }
        if (selected_resources.empty())
          continue;
        auto quiescence = observe_current_quiescence_after_refresh(
            source, selected_resources, assessed_at_ns, clock_);
        if (!quiescence.ok())
          continue;
        for (const auto &resource : selected_resources) {
          const auto settlement = resource_facts.find(resource);
          if (settlement == resource_facts.end() ||
              settlement->second.state !=
                  core::StopFaultRecoveryResourceStateDTO::kSettled ||
              !settlement->second.evidence_digest.has_value()) {
            continue;
          }
          const auto &observed = quiescence.value().observation;
          external_facts.push_back(core::StopFaultRecoveryResourceFactDTO{
              resource, core::StopFaultRecoveryResourceStateDTO::kSettled,
              observed.snapshot.source_epoch, observed.snapshot.sequence,
              observed.source_age_ns,
              kRetainedNativeReconciliationFreshnessBudgetNs,
              external_reconciliation_evidence_digest(
                  latch, resource, *settlement->second.evidence_digest,
                  observed.no_active_goal_evidence_digest,
                  observed.configuration_evidence_digest),
              std::nullopt});
        }
      }
      std::sort(external_facts.begin(), external_facts.end(),
                [](const auto &left, const auto &right) {
                  return left.resource_id < right.resource_id;
                });
    }

    std::vector<core::StopFaultRecoveryResourceFactDTO> resources;
    resources.reserve(resource_facts.size());
    for (auto &[resource, fact] : resource_facts) {
      static_cast<void>(resource);
      resources.push_back(std::move(fact));
    }
    core::StopFaultRecoveryFactsDTO facts{assessed_at_ns,
                                          latch.cell_id,
                                          latch.retained_bundle_id,
                                          latch.graph_revision,
                                          *status.stop_certificate,
                                          latch.latch_id,
                                          latch.latch_generation,
                                          status.acceptance_projection,
                                          status.terminal_projection,
                                          status.surveillance_projection,
                                          status.surveillance,
                                          std::move(resources),
                                          std::move(configuration_facts),
                                          {},
                                          std::move(external_facts),
                                          {},
                                          {},
                                          {}};
    if (core::stop_fault_recovery_facts_field_error(facts).has_value()) {
      return invalid<core::StopFaultRecoveryFactsDTO>(
          "native_stop_recovery_facts_invalid",
          "native Stop recovery facts are incomplete or inconsistent");
    }
    return core::Result<core::StopFaultRecoveryFactsDTO>::Ok(std::move(facts));
  } catch (const std::exception &exception) {
    return invalid<core::StopFaultRecoveryFactsDTO>(
        "native_stop_recovery_facts_failed", exception.what());
  } catch (...) {
    return invalid<core::StopFaultRecoveryFactsDTO>(
        "native_stop_recovery_facts_failed",
        "native Stop recovery facts construction failed");
  }
}

core::Result<core::OrphanResourceReconciliationFactsDTO>
RetainedStopSurveillanceOwner::BuildOrphanResourceReconciliationFacts(
    const core::RequestOwnerEpoch &request_owner_epoch,
    const core::CellId &cell_id,
    const std::uint64_t expected_operation_resource_generation,
    const std::vector<core::ResourceId> &resources,
    const std::int64_t assessed_at_ns) noexcept {
  try {
    if (request_owner_epoch.empty() || cell_id.empty() || assessed_at_ns < 0 ||
        resources.empty() ||
        resources.size() > core::kMaximumOrphanResourceReconciliationFacts ||
        !std::is_sorted(resources.begin(), resources.end()) ||
        std::adjacent_find(resources.begin(), resources.end()) !=
            resources.end()) {
      return invalid<core::OrphanResourceReconciliationFactsDTO>(
          "native_orphan_reconciliation_request_invalid",
          "native orphan reconciliation request is invalid");
    }
    std::vector<RetainedStopSurveillanceSourceInput> sources;
    {
      std::lock_guard lock(mutex_);
      sources = sources_;
    }
    std::map<core::ResourceId, std::size_t> source_by_resource;
    for (std::size_t index = 0U; index < sources.size(); ++index) {
      for (const auto &resource : binding_resources(sources[index].binding)) {
        if (!source_by_resource.emplace(resource, index).second) {
          return invalid<core::OrphanResourceReconciliationFactsDTO>(
              "native_orphan_reconciliation_source_ambiguous",
              "native orphan reconciliation resource has multiple sources");
        }
      }
    }
    std::map<std::size_t, std::vector<core::ResourceId>> resources_by_source;
    for (const auto &resource : resources) {
      const auto found = source_by_resource.find(resource);
      if (found == source_by_resource.end()) {
        return invalid<core::OrphanResourceReconciliationFactsDTO>(
            "native_orphan_reconciliation_source_missing",
            "native orphan reconciliation resource has no retained source");
      }
      resources_by_source[found->second].push_back(resource);
    }

    struct PendingObservation final {
      std::size_t source_index{};
      std::vector<core::ResourceId> resources;
      CurrentQuiescenceObservation observation;
    };
    std::vector<PendingObservation> pending;
    pending.reserve(resources_by_source.size());
    auto effective_assessed_at_ns = assessed_at_ns;
    for (const auto &[source_index, selected_resources] : resources_by_source) {
      const auto &source = sources[source_index];
      auto observed = observe_current_quiescence_after_refresh(
          source, selected_resources, assessed_at_ns, clock_);
      if (!observed.ok()) {
        return core::Result<core::OrphanResourceReconciliationFactsDTO>::Err(
            observed.error());
      }
      auto refreshed = std::move(observed.value());
      effective_assessed_at_ns =
          std::max(effective_assessed_at_ns, refreshed.assessed_at_ns);
      pending.push_back(PendingObservation{
          source_index, selected_resources,
          std::move(refreshed.observation)});
    }

    std::vector<core::OrphanResourceReconciliationFactDTO> facts;
    facts.reserve(resources.size());
    for (auto &pending_observation : pending) {
      const auto &source = sources[pending_observation.source_index];
      const auto &observed = pending_observation.observation;
      const auto source_age_ns =
          effective_assessed_at_ns - observed.snapshot.received_timestamp_ns;
      if (source_age_ns < 0 ||
          source_age_ns > kRetainedNativeReconciliationFreshnessBudgetNs) {
        return invalid<core::OrphanResourceReconciliationFactsDTO>(
            "native_current_resource_state_stale",
            "native current-resource state became stale before reconciliation");
      }
      const auto &binding = source.binding.value();
      const auto &path = *binding.control_path;
      for (const auto &resource : pending_observation.resources) {
        core::OrphanResourceReconciliationFactDTO fact{
            resource,
            binding.controllable_component_id,
            binding.command_target_id,
            path.control_domain_id,
            path.control_binding_id,
            path.control_binding_revision,
            path.active_control_configuration_id,
            path.active_control_configuration_generation,
            path.provider_runtime_unit_id,
            path.provider_runtime_epoch,
            core::StopFaultRecoveryResourceFactDTO{
                resource, core::StopFaultRecoveryResourceStateDTO::kSettled,
                observed.snapshot.source_epoch,
                observed.snapshot.sequence,
                source_age_ns,
                kRetainedNativeReconciliationFreshnessBudgetNs, std::nullopt,
                std::nullopt},
            observed.no_active_goal_evidence_digest,
            observed.configuration_evidence_digest};
        fact.settlement.evidence_digest = current_resource_evidence_digest(
            fact, observed.snapshot, observed.no_active_goal_evidence_digest,
            observed.configuration_evidence_digest, effective_assessed_at_ns);
        facts.push_back(std::move(fact));
      }
    }
    std::sort(facts.begin(), facts.end(),
              [](const auto &left, const auto &right) {
                return left.resource_id < right.resource_id;
              });
    const auto &value = sources.front().binding.value();
    core::OrphanResourceReconciliationFactsDTO result{
        request_owner_epoch,
        cell_id,
        value.retained_bundle_id,
        value.graph_revision,
        expected_operation_resource_generation,
        effective_assessed_at_ns,
        std::move(facts)};
    if (core::orphan_resource_reconciliation_facts_field_error(result)
            .has_value()) {
      return invalid<core::OrphanResourceReconciliationFactsDTO>(
          "native_orphan_reconciliation_facts_invalid",
          "native orphan reconciliation facts are inconsistent");
    }
    return core::Result<core::OrphanResourceReconciliationFactsDTO>::Ok(
        std::move(result));
  } catch (const std::exception &exception) {
    return invalid<core::OrphanResourceReconciliationFactsDTO>(
        "native_orphan_reconciliation_failed", exception.what());
  } catch (...) {
    return invalid<core::OrphanResourceReconciliationFactsDTO>(
        "native_orphan_reconciliation_failed",
        "native orphan reconciliation facts construction failed");
  }
}

} // namespace manyforge::roboplan_backend
