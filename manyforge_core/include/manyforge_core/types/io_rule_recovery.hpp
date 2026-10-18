#pragma once

#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_core/types/io_rule_service_configuration.hpp>
#include <optional>
#include <string>

namespace manyforge::core {

/// Exact global latch occurrence in the existing Kernel rule row. The nonce
/// is the actual signal-service registration that acquired the latch, not a
/// new counter. Together with Applied/catalog identity it fences rearm/ABA.
struct IoGlobalRuleRecoverySelectorDTO final {
  IoRuleStateContextDTO context;
  InterruptPolicyId rule_id;
  std::uint64_t expected_rule_registration_id;
  friend bool operator==(const IoGlobalRuleRecoverySelectorDTO&,
                         const IoGlobalRuleRecoverySelectorDTO&) = default;
};

/// Additive typed selector for the existing operator cell-recovery command.
/// It never stands in for a Stop ID, certificate or operation Reset.
struct IoGlobalRuleRecoveryIntentDTO final {
  PublicRequestContextDTO request_context;
  IoGlobalRuleRecoverySelectorDTO selector;
  std::optional<std::string> reason{};
  friend bool operator==(const IoGlobalRuleRecoveryIntentDTO&,
                         const IoGlobalRuleRecoveryIntentDTO&) = default;
};

enum class IoGlobalRuleLatchCauseDTO { kCondition, kSourceLoss };

/// Published only for an actually latched row. Unknown condition is null,
/// never false or permission to clear. The action re-evaluates current source
/// qualification and exact selector; this observation alone grants nothing.
struct IoGlobalRuleRecoveryOfferDTO final {
  IoGlobalRuleRecoverySelectorDTO selector;
  std::optional<bool> current_condition;
  InterruptPostLatchDisposition post_latch_disposition;
  IoGlobalRuleLatchCauseDTO latch_cause{IoGlobalRuleLatchCauseDTO::kCondition};
  friend bool operator==(const IoGlobalRuleRecoveryOfferDTO&,
                         const IoGlobalRuleRecoveryOfferDTO&) = default;
};

struct IoGlobalRuleRecoveryResultDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  bool accepted;
  IoRuleStateContextDTO context;
  IoRuleStateDTO rule;
  std::optional<ErrorDTO> error{};
  friend bool operator==(const IoGlobalRuleRecoveryResultDTO&,
                         const IoGlobalRuleRecoveryResultDTO&) = default;
};

/// Exact immutable authorization/history payload. It is not a rule-state
/// mirror event; the actual clear/successor commit publishes that separately.
struct IoGlobalRuleRecoveryRecordDTO final {
  IoGlobalRuleRecoveryIntentDTO intent;
  IoGlobalRuleRecoveryResultDTO result;
  IoRuleStateDTO prior_latched_state;
  IoRuleEvidenceDTO prior_latched_evidence;
  IoRuleConditionSnapshotDTO inactive_observation;
  IoRuleRegistrationDTO successor_registration;
  /// An actual loss may upgrade an existing condition latch after its original
  /// observer was retired. Preserve both real occurrences without relabeling.
  std::optional<IoRuleEvidenceDTO> source_loss_evidence{};
  friend bool operator==(const IoGlobalRuleRecoveryRecordDTO&,
                         const IoGlobalRuleRecoveryRecordDTO&) = default;
};

}  // namespace manyforge::core
