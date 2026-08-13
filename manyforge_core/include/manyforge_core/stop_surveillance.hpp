#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/stop_terminal_state.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kStopLateQuiescenceEvidenceIdentityDomain =
    "stop-late-quiescence-evidence-digest";
inline constexpr std::string_view kStopLateQuiescenceEvidenceApiVersion =
    "manyforge.stop-late-quiescence-evidence.v1";

/// One immutable evidence-source row retained by the terminal certificate and
/// selected for post-terminal observation. This is private owner input, never
/// an author-selected runtime source.
struct StopSurveillanceSourcePlanDTO final {
  StopScopeEntryKeyDTO scope_key;
  StopEvidenceRequirementDTO evidence_requirement;
  ResolvedStopEvidenceSourceDTO resolved_source;

  friend bool operator==(const StopSurveillanceSourcePlanDTO&,
                         const StopSurveillanceSourcePlanDTO&) = default;
};

struct StopSurveillanceObservationQueryDTO final {
  StopCertificateRefDTO stop_certificate;
  StopScopeEntryKeyDTO scope_key;
  StopEvidenceRequirementDTO evidence_requirement;
  ResolvedStopEvidenceSourceDTO resolved_source;
  std::int64_t evaluation_time_ns{};

  friend bool operator==(const StopSurveillanceObservationQueryDTO&,
                         const StopSurveillanceObservationQueryDTO&) = default;
};

struct StopSurveillanceObservationSampleDTO final {
  StopCertificateRefDTO stop_certificate;
  StopScopeEntryKeyDTO scope_key;
  StopEvidenceItemId evidence_item_id;
  ResolvedStopEvidenceSourceDTO resolved_source;
  StopSurveillanceSourceAvailabilityDTO availability{
      StopSurveillanceSourceAvailabilityDTO::kUnavailable};
  std::optional<StopEvidenceItemDTO> evaluated_evidence;
  std::optional<ErrorDTO> error;

  friend bool operator==(const StopSurveillanceObservationSampleDTO&,
                         const StopSurveillanceObservationSampleDTO&) = default;
};

/// Pure reducer state. `latest_evidence` is owner-private continuity needed to
/// prove that every source is simultaneously fresh and satisfied; only the
/// bounded public projection is journaled.
struct StopSurveillanceReducerState final {
  StopPostTerminalSurveillanceDTO projection;
  std::vector<StopSurveillanceSourcePlanDTO> source_plans;
  std::vector<std::optional<StopEvidenceItemDTO>> latest_evidence;
  std::vector<std::optional<std::int64_t>> latest_evaluation_time_ns;

  friend bool operator==(const StopSurveillanceReducerState&,
                         const StopSurveillanceReducerState&) = default;
};

[[nodiscard]] std::optional<std::string_view> stop_surveillance_source_plan_field_error(
    const StopSurveillanceSourcePlanDTO& plan) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_surveillance_observation_query_field_error(
    const StopSurveillanceObservationQueryDTO& query) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_surveillance_observation_sample_field_error(
    const StopSurveillanceObservationQueryDTO& query,
    const StopSurveillanceObservationSampleDTO& sample) noexcept;

[[nodiscard]] Result<StopSurveillanceReducerState> MakeStopSurveillanceReducerState(
    const RequestOwnerEpoch& owner_epoch, std::uint64_t surveillance_generation,
    const StopCertificateDTO& certificate, const StopFaultLatchDTO& latch,
    const StopSurveillancePolicyDTO& policy,
    std::vector<StopSurveillanceSourcePlanDTO> source_plans);

/// Adopt one exact durable checkpoint after owner replacement. Private sample
/// bodies are intentionally not reconstructed; monitoring resumes
/// conservatively from source sequences strictly newer than the checkpoint.
[[nodiscard]] Result<StopSurveillanceReducerState> AdoptStopSurveillanceReducerState(
    const RequestOwnerEpoch& owner_epoch, const StopCertificateDTO& certificate,
    const StopFaultLatchDTO& latch, StopPostTerminalSurveillanceDTO recovered_projection,
    std::vector<StopSurveillanceSourcePlanDTO> source_plans);

[[nodiscard]] Result<StopSurveillanceObservationQueryDTO> MakeStopSurveillanceObservationQuery(
    const StopSurveillanceReducerState& state, std::size_t source_index,
    std::int64_t evaluation_time_ns);

[[nodiscard]] Result<StopSurveillanceReducerState> ReduceStopSurveillanceObservation(
    StopSurveillanceReducerState state, const StopSurveillanceObservationQueryDTO& query,
    const StopSurveillanceObservationSampleDTO& sample);

/// Materialize the next coalesced durable-checkpoint candidate. It advances no
/// observation state and does not perform journal I/O.
[[nodiscard]] Result<StopPostTerminalSurveillanceDTO> MakeStopSurveillanceCheckpoint(
    const StopSurveillanceReducerState& state);

}  // namespace manyforge::core
