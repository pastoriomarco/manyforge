#include <algorithm>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/stop_surveillance.hpp>
#include <tuple>
#include <type_traits>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

template <typename Identity>
[[nodiscard]] bool identity(const Identity& value) noexcept {
  return !value.empty() && value.value().size() <= kMaximumStopIdentityBytes;
}

[[nodiscard]] bool error_valid(const ErrorDTO& error) noexcept {
  return !error.code.empty() && error.code.size() <= 128U && error.message.size() <= 1024U &&
         error.details.size() <= 8U &&
         std::all_of(error.details.begin(), error.details.end(), [](const auto& row) {
           return !row.first.empty() && row.first.size() <= 64U && row.second.size() <= 256U;
         });
}

[[nodiscard]] bool key_less(const StopScopeEntryKeyDTO& left,
                            const StopScopeEntryKeyDTO& right) noexcept {
  if (left.origin_stop_id != right.origin_stop_id) {
    return left.origin_stop_id < right.origin_stop_id;
  }
  if (left.origin_stop_generation != right.origin_stop_generation) {
    return left.origin_stop_generation < right.origin_stop_generation;
  }
  if (left.subject.index() != right.subject.index()) {
    return left.subject.index() < right.subject.index();
  }
  if (const auto* participant = std::get_if<StopParticipantScopeKeyDTO>(&left.subject)) {
    const auto& other = std::get<StopParticipantScopeKeyDTO>(right.subject);
    return std::tie(participant->operation_id, participant->participant_id) <
           std::tie(other.operation_id, other.participant_id);
  }
  return std::get<ResourceId>(left.subject) < std::get<ResourceId>(right.subject);
}

[[nodiscard]] bool plan_less(const StopSurveillanceSourcePlanDTO& left,
                             const StopSurveillanceSourcePlanDTO& right) noexcept {
  if (key_less(left.scope_key, right.scope_key)) return true;
  if (key_less(right.scope_key, left.scope_key)) return false;
  return left.evidence_requirement.evidence_item_id < right.evidence_requirement.evidence_item_id;
}

[[nodiscard]] bool plan_joins_certificate(const StopSurveillanceSourcePlanDTO& plan,
                                          const StopCertificateDTO& certificate) noexcept {
  for (const auto& attempt : certificate.attempted_stage_results) {
    if (attempt.scope_key != plan.scope_key) continue;
    for (const auto& command : attempt.result.request.participant_stop_plan.stage_commands) {
      for (const auto& evidence : command.resolved_stage.evidence_requirements) {
        if (evidence.requirement == plan.evidence_requirement &&
            evidence.resolved_source == plan.resolved_source) {
          return true;
        }
      }
    }
  }
  return false;
}

[[nodiscard]] CanonicalValue json_value(const JsonValueDTO& value) {
  return std::visit(
      [](const auto& selected) -> CanonicalValue {
        using T = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<T, bool>) {
          return CanonicalValue(selected);
        } else if constexpr (std::is_same_v<T, double>) {
          return CanonicalValue(selected);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return CanonicalValue(selected);
        } else if constexpr (std::is_same_v<T, JsonValueDTO::Array>) {
          CanonicalValue::Array rows;
          rows.reserve(selected.size());
          for (const auto& row : selected) rows.emplace_back(json_value(row));
          return CanonicalValue(std::move(rows));
        } else {
          CanonicalValue::Object rows;
          rows.reserve(selected.size());
          for (const auto& [key, row] : selected) rows.emplace_back(key, json_value(row));
          return CanonicalValue(std::move(rows));
        }
      },
      value.value);
}

[[nodiscard]] ContentDigest late_quiescence_digest(
    const StopPostTerminalSurveillanceDTO& projection,
    const std::vector<StopSurveillanceSourcePlanDTO>& plans,
    const std::vector<std::optional<StopEvidenceItemDTO>>& evidence) {
  CanonicalValue::Array rows;
  rows.reserve(plans.size());
  for (std::size_t index = 0U; index < plans.size(); ++index) {
    const auto& item = *evidence[index];
    rows.emplace_back(CanonicalValue::Object{
        {"evidence",
         item.evidence.has_value() ? json_value(*item.evidence) : CanonicalValue(nullptr)},
        {"evidence_item_id", CanonicalValue(item.evidence_item_id.value())},
        {"scope_origin_generation", CanonicalValue(static_cast<double>(
                                        plans[index].scope_key.origin_stop_generation.value()))},
        {"scope_origin_stop_id", CanonicalValue(plans[index].scope_key.origin_stop_id.value())},
        {"source_epoch", CanonicalValue(item.source_epoch->value())},
        {"source_sequence", CanonicalValue(static_cast<double>(*item.source_sequence))},
    });
  }
  const CanonicalValue value(CanonicalValue::Object{
      {"latch_generation",
       CanonicalValue(static_cast<double>(projection.latch_generation.value()))},
      {"latch_id", CanonicalValue(projection.latch_id.value())},
      {"sources", CanonicalValue(std::move(rows))},
      {"stop_certificate_id",
       CanonicalValue(projection.stop_certificate.stop_certificate_id.value())},
      {"stop_certificate_revision",
       CanonicalValue(
           static_cast<double>(projection.stop_certificate.stop_certificate_revision.value()))},
  });
  return ContentDigest(content_identity::framed_sha256(
      kStopLateQuiescenceEvidenceIdentityDomain, kStopLateQuiescenceEvidenceApiVersion, value));
}

template <typename T>
[[nodiscard]] Result<T> invalid(std::string code, std::string message) {
  return Result<T>::Err(ErrorDTO{std::move(code), std::move(message), {}});
}

[[nodiscard]] std::optional<std::size_t> plan_index(
    const StopSurveillanceReducerState& state,
    const StopSurveillanceObservationQueryDTO& query) noexcept {
  const StopSurveillanceSourcePlanDTO needle{query.scope_key, query.evidence_requirement,
                                             query.resolved_source};
  const auto found =
      std::lower_bound(state.source_plans.begin(), state.source_plans.end(), needle, plan_less);
  if (found == state.source_plans.end() || *found != needle) return std::nullopt;
  return static_cast<std::size_t>(found - state.source_plans.begin());
}

[[nodiscard]] StopCertificateRefDTO certificate_ref(const StopCertificateDTO& certificate) {
  return StopCertificateRefDTO{certificate.stop_certificate_id,
                               certificate.stop_certificate_revision, certificate.stop_id,
                               certificate.stop_generation};
}

}  // namespace

std::optional<std::string_view> stop_surveillance_source_plan_field_error(
    const StopSurveillanceSourcePlanDTO& plan) noexcept {
  if (!identity(plan.scope_key.origin_stop_id) ||
      !plan.scope_key.origin_stop_generation.nonzero() ||
      !identity(plan.evidence_requirement.evidence_item_id) ||
      !identity(plan.evidence_requirement.phase_id) ||
      plan.evidence_requirement.source_requirement.freshness_budget_ns <= 0 ||
      plan.evidence_requirement.source_requirement.source_kind !=
          plan.resolved_source.source_kind ||
      !identity(plan.resolved_source.runtime_target.provider_instance_id) ||
      !identity(plan.resolved_source.runtime_target.runtime_unit_id) ||
      !identity(plan.resolved_source.runtime_target.provider_runtime_epoch)) {
    return "stop_surveillance_source_plan_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> stop_surveillance_observation_query_field_error(
    const StopSurveillanceObservationQueryDTO& query) noexcept {
  const StopSurveillanceSourcePlanDTO plan{query.scope_key, query.evidence_requirement,
                                           query.resolved_source};
  if (!query.stop_certificate.locally_valid() || query.evaluation_time_ns < 0 ||
      stop_surveillance_source_plan_field_error(plan).has_value()) {
    return "stop_surveillance_observation_query_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> stop_surveillance_observation_sample_field_error(
    const StopSurveillanceObservationQueryDTO& query,
    const StopSurveillanceObservationSampleDTO& sample) noexcept {
  if (stop_surveillance_observation_query_field_error(query).has_value() ||
      sample.stop_certificate != query.stop_certificate || sample.scope_key != query.scope_key ||
      sample.evidence_item_id != query.evidence_requirement.evidence_item_id ||
      sample.resolved_source != query.resolved_source) {
    return "stop_surveillance_observation_sample_join_invalid";
  }
  switch (sample.availability) {
    case StopSurveillanceSourceAvailabilityDTO::kAvailable: {
      if (!sample.evaluated_evidence.has_value() || sample.error.has_value()) {
        return "stop_surveillance_available_sample_presence_invalid";
      }
      const auto& item = *sample.evaluated_evidence;
      if (item.evidence_item_id != query.evidence_requirement.evidence_item_id ||
          item.required_class != query.evidence_requirement.evidence_kind ||
          item.resolved_source != query.resolved_source ||
          (item.state != StopEvidenceItemStateDTO::kSatisfied &&
           item.state != StopEvidenceItemStateDTO::kFailed) ||
          !item.source_epoch.has_value() || !item.source_sequence.has_value() ||
          !item.source_age_at_evaluation_ns.has_value() ||
          *item.source_epoch != query.resolved_source.runtime_target.provider_runtime_epoch ||
          *item.source_sequence == 0U ||
          *item.source_sequence >
              static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
          *item.source_age_at_evaluation_ns < 0 ||
          *item.source_age_at_evaluation_ns >
              query.evidence_requirement.source_requirement.freshness_budget_ns ||
          (item.state == StopEvidenceItemStateDTO::kSatisfied &&
           (!item.evidence.has_value() || item.error.has_value())) ||
          (item.state == StopEvidenceItemStateDTO::kFailed && !item.error.has_value()) ||
          (item.error.has_value() && !error_valid(*item.error))) {
        return "stop_surveillance_available_sample_invalid";
      }
      return std::nullopt;
    }
    case StopSurveillanceSourceAvailabilityDTO::kUnavailable:
    case StopSurveillanceSourceAvailabilityDTO::kInvalid:
      return !sample.evaluated_evidence.has_value() && sample.error.has_value() &&
                     error_valid(*sample.error)
                 ? std::nullopt
                 : std::optional<std::string_view>(
                       "stop_surveillance_unavailable_sample_presence_invalid");
  }
  return "stop_surveillance_observation_sample_state_invalid";
}

Result<StopSurveillanceReducerState> MakeStopSurveillanceReducerState(
    const RequestOwnerEpoch& owner_epoch, const std::uint64_t surveillance_generation,
    const StopCertificateDTO& certificate, const StopFaultLatchDTO& latch,
    const StopSurveillancePolicyDTO& policy,
    std::vector<StopSurveillanceSourcePlanDTO> source_plans) {
  const auto reference = certificate_ref(certificate);
  if (!identity(owner_epoch) || surveillance_generation == 0U ||
      stop_certificate_field_error(certificate).has_value() ||
      stop_fault_latch_field_error(latch).has_value() || !latch.stop_certificate.has_value() ||
      *latch.stop_certificate != reference || latch.state == StopFaultLatchStateDTO::kCleared ||
      policy.sample_cadence_ns <= 0 || policy.checkpoint_interval_ns < policy.sample_cadence_ns ||
      policy.retained_checkpoint_limit == 0U ||
      policy.retained_checkpoint_limit > kMaximumStopSurveillanceCheckpoints ||
      source_plans.empty() || source_plans.size() > kMaximumStopSurveillanceSources ||
      (certificate.result != StopCertificateResultDTO::kPartial &&
       certificate.result != StopCertificateResultDTO::kFailedUncontained &&
       latch.trigger != StopFaultTriggerDTO::kPhysicalRestUnproven)) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_seed_invalid",
                                                 "Stop surveillance seed is invalid");
  }
  if (!std::is_sorted(source_plans.begin(), source_plans.end(), plan_less) ||
      std::adjacent_find(source_plans.begin(), source_plans.end(),
                         [](const auto& left, const auto& right) {
                           return !plan_less(left, right) && !plan_less(right, left);
                         }) != source_plans.end() ||
      !std::all_of(source_plans.begin(), source_plans.end(), [&](const auto& plan) {
        return !stop_surveillance_source_plan_field_error(plan).has_value() &&
               plan_joins_certificate(plan, certificate);
      })) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_source_plan_invalid",
                                                 "Stop surveillance sources are not canonical");
  }

  std::vector<StopSurveillanceSourceStatusDTO> statuses;
  statuses.reserve(source_plans.size());
  for (const auto& plan : source_plans) {
    statuses.push_back(StopSurveillanceSourceStatusDTO{
        plan.scope_key, plan.evidence_requirement.evidence_item_id, plan.resolved_source,
        StopSurveillanceSourceAvailabilityDTO::kUnavailable, std::nullopt, std::nullopt,
        std::nullopt,
        ErrorDTO{"stop_surveillance_observation_pending",
                 "Post-terminal evidence source has not produced a fresh sample",
                 {}}});
  }
  StopPostTerminalSurveillanceDTO projection{
      certificate.stop_id,
      certificate.stop_generation,
      reference,
      latch.latch_id,
      latch.latch_generation,
      owner_epoch,
      surveillance_generation,
      latch.captured_resources,
      std::move(statuses),
      policy,
      0U,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      StopPostTerminalSurveillanceStateDTO::kMonitoring,
      1U,
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                     std::nullopt}};
  if (stop_post_terminal_surveillance_field_error(projection).has_value()) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_seed_invalid",
                                                 "Stop surveillance projection is invalid");
  }
  const auto source_count = projection.source_statuses.size();
  return Result<StopSurveillanceReducerState>::Ok(
      StopSurveillanceReducerState{std::move(projection), std::move(source_plans),
                                   std::vector<std::optional<StopEvidenceItemDTO>>(source_count),
                                   std::vector<std::optional<std::int64_t>>(source_count)});
}

Result<StopSurveillanceReducerState> AdoptStopSurveillanceReducerState(
    const RequestOwnerEpoch& owner_epoch, const StopCertificateDTO& certificate,
    const StopFaultLatchDTO& latch, StopPostTerminalSurveillanceDTO recovered_projection,
    std::vector<StopSurveillanceSourcePlanDTO> source_plans) {
  const auto reference = certificate_ref(certificate);
  if (!identity(owner_epoch) || stop_certificate_field_error(certificate).has_value() ||
      stop_fault_latch_field_error(latch).has_value() || !latch.stop_certificate.has_value() ||
      *latch.stop_certificate != reference || latch.state == StopFaultLatchStateDTO::kCleared ||
      stop_post_terminal_surveillance_field_error(recovered_projection).has_value() ||
      recovered_projection.stop_certificate != reference ||
      recovered_projection.latch_id != latch.latch_id ||
      recovered_projection.latch_generation != latch.latch_generation ||
      recovered_projection.resources != latch.captured_resources ||
      recovered_projection.checkpoint_projection.state !=
          StopDurableProjectionStateDTO::kRecorded ||
      source_plans.size() != recovered_projection.source_statuses.size() ||
      !std::is_sorted(source_plans.begin(), source_plans.end(), plan_less) ||
      std::adjacent_find(source_plans.begin(), source_plans.end(),
                         [](const auto& left, const auto& right) {
                           return !plan_less(left, right) && !plan_less(right, left);
                         }) != source_plans.end()) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_adoption_invalid",
                                                 "Stop surveillance checkpoint is invalid");
  }
  for (std::size_t index = 0U; index < source_plans.size(); ++index) {
    const auto& plan = source_plans[index];
    const auto& status = recovered_projection.source_statuses[index];
    if (stop_surveillance_source_plan_field_error(plan).has_value() ||
        !plan_joins_certificate(plan, certificate) || status.scope_key != plan.scope_key ||
        status.evidence_item_id != plan.evidence_requirement.evidence_item_id ||
        status.resolved_source != plan.resolved_source) {
      return invalid<StopSurveillanceReducerState>("stop_surveillance_adoption_invalid",
                                                   "Stop surveillance sources do not join");
    }
  }
  recovered_projection.owner_epoch = owner_epoch;
  const auto source_count = source_plans.size();
  return Result<StopSurveillanceReducerState>::Ok(
      StopSurveillanceReducerState{std::move(recovered_projection), std::move(source_plans),
                                   std::vector<std::optional<StopEvidenceItemDTO>>(source_count),
                                   std::vector<std::optional<std::int64_t>>(source_count)});
}

Result<StopSurveillanceObservationQueryDTO> MakeStopSurveillanceObservationQuery(
    const StopSurveillanceReducerState& state, const std::size_t source_index,
    const std::int64_t evaluation_time_ns) {
  if (source_index >= state.source_plans.size() || evaluation_time_ns < 0 ||
      state.projection.state != StopPostTerminalSurveillanceStateDTO::kMonitoring) {
    return invalid<StopSurveillanceObservationQueryDTO>("stop_surveillance_query_unavailable",
                                                        "Stop surveillance query is unavailable");
  }
  const auto& plan = state.source_plans[source_index];
  StopSurveillanceObservationQueryDTO query{state.projection.stop_certificate, plan.scope_key,
                                            plan.evidence_requirement, plan.resolved_source,
                                            evaluation_time_ns};
  if (stop_surveillance_observation_query_field_error(query).has_value()) {
    return invalid<StopSurveillanceObservationQueryDTO>(
        "stop_surveillance_observation_query_invalid", "Stop surveillance query is invalid");
  }
  return Result<StopSurveillanceObservationQueryDTO>::Ok(std::move(query));
}

Result<StopSurveillanceReducerState> ReduceStopSurveillanceObservation(
    StopSurveillanceReducerState state, const StopSurveillanceObservationQueryDTO& query,
    const StopSurveillanceObservationSampleDTO& sample) {
  const auto index = plan_index(state, query);
  if (!index.has_value() ||
      state.projection.state != StopPostTerminalSurveillanceStateDTO::kMonitoring ||
      stop_surveillance_observation_sample_field_error(query, sample).has_value()) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_observation_invalid",
                                                 "Stop surveillance observation is invalid");
  }
  auto& prior_status = state.projection.source_statuses[*index];
  auto& prior_evidence = state.latest_evidence[*index];
  auto& prior_evaluation_time_ns = state.latest_evaluation_time_ns[*index];
  if (sample.availability == StopSurveillanceSourceAvailabilityDTO::kAvailable &&
      !prior_evidence.has_value() && prior_status.source_epoch.has_value() &&
      prior_status.source_sequence.has_value() &&
      *prior_status.source_epoch == *sample.evaluated_evidence->source_epoch &&
      *sample.evaluated_evidence->source_sequence <= *prior_status.source_sequence) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_observation_stale",
                                                 "Stop surveillance sequence is not newer");
  }
  if (sample.availability == StopSurveillanceSourceAvailabilityDTO::kAvailable &&
      prior_evidence.has_value() &&
      prior_evidence->source_epoch == sample.evaluated_evidence->source_epoch &&
      prior_evidence->source_sequence == sample.evaluated_evidence->source_sequence) {
    if (*prior_evidence == *sample.evaluated_evidence && prior_evaluation_time_ns.has_value() &&
        *prior_evaluation_time_ns == query.evaluation_time_ns) {
      return Result<StopSurveillanceReducerState>::Ok(std::move(state));
    }
    return invalid<StopSurveillanceReducerState>("stop_surveillance_observation_conflict",
                                                 "Stop surveillance sequence conflicts");
  }
  if (sample.availability == StopSurveillanceSourceAvailabilityDTO::kAvailable &&
      prior_evidence.has_value() &&
      sample.evaluated_evidence->source_sequence <= prior_evidence->source_sequence) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_observation_stale",
                                                 "Stop surveillance sequence is stale");
  }
  if (state.projection.surveillance_generation == std::numeric_limits<std::uint64_t>::max()) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_generation_exhausted",
                                                 "Stop surveillance generation is exhausted");
  }

  ++state.projection.surveillance_generation;
  if (state.projection.checkpoint_projection.state != StopDurableProjectionStateDTO::kPending) {
    if (state.projection.checkpoint_generation == std::numeric_limits<std::uint64_t>::max()) {
      return invalid<StopSurveillanceReducerState>("stop_surveillance_checkpoint_exhausted",
                                                   "Stop checkpoint generation is exhausted");
    }
    ++state.projection.checkpoint_generation;
  }
  state.projection.checkpoint_projection = StopDurableProjectionStatusDTO{
      StopDurableProjectionStateDTO::kPending, std::nullopt, std::nullopt};

  if (sample.availability == StopSurveillanceSourceAvailabilityDTO::kAvailable) {
    const auto& item = *sample.evaluated_evidence;
    prior_status =
        StopSurveillanceSourceStatusDTO{query.scope_key,
                                        item.evidence_item_id,
                                        query.resolved_source,
                                        StopSurveillanceSourceAvailabilityDTO::kAvailable,
                                        item.source_epoch,
                                        item.source_sequence,
                                        item.source_age_at_evaluation_ns,
                                        std::nullopt};
    prior_evidence = item;
    prior_evaluation_time_ns = query.evaluation_time_ns;
    if (item.state == StopEvidenceItemStateDTO::kFailed) {
      if (state.projection.continued_motion_count == std::numeric_limits<std::uint64_t>::max()) {
        return invalid<StopSurveillanceReducerState>(
            "stop_surveillance_observation_capacity_exhausted",
            "Stop surveillance continued-effect count is exhausted");
      }
      ++state.projection.continued_motion_count;
      if (!state.projection.first_continued_motion_at_ns.has_value()) {
        state.projection.first_continued_motion_at_ns = query.evaluation_time_ns;
      }
      state.projection.last_continued_motion_at_ns = query.evaluation_time_ns;
    }
  } else {
    prior_status = StopSurveillanceSourceStatusDTO{
        query.scope_key,       query.evidence_requirement.evidence_item_id,
        query.resolved_source, sample.availability,
        std::nullopt,          std::nullopt,
        std::nullopt,          sample.error};
    prior_evidence.reset();
    prior_evaluation_time_ns.reset();
  }

  bool quiescent = true;
  for (std::size_t candidate = 0U; candidate < state.source_plans.size(); ++candidate) {
    const auto& item = state.latest_evidence[candidate];
    const auto& evaluated_at_ns = state.latest_evaluation_time_ns[candidate];
    const auto& status = state.projection.source_statuses[candidate];
    const auto freshness_budget_ns =
        state.source_plans[candidate].evidence_requirement.source_requirement.freshness_budget_ns;
    if (!item.has_value() || !evaluated_at_ns.has_value() ||
        item->state != StopEvidenceItemStateDTO::kSatisfied ||
        status.availability != StopSurveillanceSourceAvailabilityDTO::kAvailable ||
        query.evaluation_time_ns < *evaluated_at_ns ||
        *item->source_age_at_evaluation_ns >
            freshness_budget_ns - (query.evaluation_time_ns - *evaluated_at_ns)) {
      quiescent = false;
      break;
    }
  }
  if (quiescent) {
    state.projection.state = StopPostTerminalSurveillanceStateDTO::kQuiescenceObserved;
    state.projection.late_quiescence_observed_at_ns = query.evaluation_time_ns;
    state.projection.late_quiescence_evidence_digest =
        late_quiescence_digest(state.projection, state.source_plans, state.latest_evidence);
  }
  if (stop_post_terminal_surveillance_field_error(state.projection).has_value()) {
    return invalid<StopSurveillanceReducerState>("stop_surveillance_reduction_invalid",
                                                 "Reduced Stop surveillance is invalid");
  }
  return Result<StopSurveillanceReducerState>::Ok(std::move(state));
}

Result<StopPostTerminalSurveillanceDTO> MakeStopSurveillanceCheckpoint(
    const StopSurveillanceReducerState& state) {
  if (stop_post_terminal_surveillance_field_error(state.projection).has_value() ||
      state.projection.checkpoint_projection.state != StopDurableProjectionStateDTO::kPending) {
    return invalid<StopPostTerminalSurveillanceDTO>(
        "stop_surveillance_checkpoint_unavailable",
        "Stop surveillance has no valid pending checkpoint");
  }
  return Result<StopPostTerminalSurveillanceDTO>::Ok(state.projection);
}

}  // namespace manyforge::core
