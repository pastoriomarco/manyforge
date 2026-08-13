#pragma once

#include <cstdint>
#include <manyforge_core/stop_terminal_state.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

enum class StopFaultRecoveryResourceStateDTO : std::uint8_t {
  kSettled = 0,
  kUnsettled,
  kUnavailable,
};

struct StopFaultRecoveryResourceFactDTO final {
  ResourceId resource_id;
  StopFaultRecoveryResourceStateDTO state{StopFaultRecoveryResourceStateDTO::kUnavailable};
  std::optional<RuntimeEpoch> source_epoch;
  std::optional<std::uint64_t> source_sequence;
  std::optional<std::int64_t> source_age_ns;
  std::optional<std::int64_t> freshness_budget_ns;
  std::optional<ContentDigest> evidence_digest;
  std::optional<ErrorDTO> error;

  friend bool operator==(const StopFaultRecoveryResourceFactDTO&,
                         const StopFaultRecoveryResourceFactDTO&) = default;
};

struct StopFaultRecoveryConfigurationFactDTO final {
  StopFaultRecoveryConfigurationExpectationDTO configuration;
  bool reconciled{};
  std::optional<ContentDigest> evidence_digest;

  friend bool operator==(const StopFaultRecoveryConfigurationFactDTO&,
                         const StopFaultRecoveryConfigurationFactDTO&) = default;
};

struct StopFaultRecoveryResetFactDTO final {
  ContentDigest requirement_digest;
  bool reconciled{};
  std::optional<ContentDigest> evidence_digest;

  friend bool operator==(const StopFaultRecoveryResetFactDTO&,
                         const StopFaultRecoveryResetFactDTO&) = default;
};

/// Private current-facts closure built by Kernel from owner-issued sources.
/// It is deliberately not a public request or authoring DTO.
struct StopFaultRecoveryFactsDTO final {
  std::int64_t assessed_at_ns{};
  CellId cell_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  StopCertificateRefDTO stop_certificate;
  StopFaultLatchId latch_id;
  StopFaultLatchGeneration latch_generation;
  StopDurableProjectionStatusDTO acceptance_projection;
  StopDurableProjectionStatusDTO terminal_projection;
  StopDurableProjectionStatusDTO surveillance_projection;
  std::optional<StopPostTerminalSurveillanceDTO> surveillance;
  std::vector<StopFaultRecoveryResourceFactDTO> resource_facts;
  std::vector<StopFaultRecoveryConfigurationFactDTO> configuration_facts;
  std::vector<StopFaultRecoveryResetFactDTO> reset_facts;
  std::vector<StopFaultRecoveryResourceFactDTO> external_reconciliation_facts;
  std::vector<StopCertificateRefDTO> blocking_stop_certificates;
  std::vector<OperationId> effect_uncertain_operation_ids;
  std::vector<ResourceId> fenced_or_orphan_resources;

  friend bool operator==(const StopFaultRecoveryFactsDTO&,
                         const StopFaultRecoveryFactsDTO&) = default;
};

/// Pure decision. An accepted result and resulting status are unpublished
/// post-commit candidates until the journal's durable-clear transaction
/// succeeds.
struct StopFaultRecoveryEvaluationDTO final {
  StopFaultRecoveryResultDTO result;
  std::optional<StopStatusDTO> resulting_stop_status;

  friend bool operator==(const StopFaultRecoveryEvaluationDTO&,
                         const StopFaultRecoveryEvaluationDTO&) = default;
};

[[nodiscard]] std::optional<std::string_view> stop_fault_recovery_intent_field_error(
    const StopFaultRecoveryIntentDTO& intent) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_fault_recovery_facts_field_error(
    const StopFaultRecoveryFactsDTO& facts) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_fault_recovery_evaluation_field_error(
    const StopFaultRecoveryEvaluationDTO& evaluation) noexcept;

[[nodiscard]] Result<StopFaultLatchDTO> AssessStopFaultRecovery(
    const StopStatusDTO& terminal_status, const StopFaultRecoveryFactsDTO& facts);

[[nodiscard]] Result<StopFaultRecoveryEvaluationDTO> EvaluateStopFaultRecovery(
    const StopFaultRecoveryIntentDTO& intent, const StopStatusDTO& terminal_status,
    const StopFaultRecoveryFactsDTO& facts);

}  // namespace manyforge::core
