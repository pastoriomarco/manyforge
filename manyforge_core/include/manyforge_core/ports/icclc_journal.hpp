#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/route_selection.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/types/cclc.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/lifecycle_target.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumCCLCRequestMembership = 4096U;
inline constexpr std::size_t kMaximumCCLCActiveTransactions = 64U;
inline constexpr std::size_t kMaximumCCLCComponentWatermarks = 64U;
inline constexpr std::size_t kMaximumCCLCActiveConfigurationWatermarks = 64U;

/// Current owner/publication scope for an explicit target retirement. Echoed
/// offer values are CAS data, never effect permission or settlement proof.
struct CCLCLifecycleTargetRetirementScopeDTO final {
  RequestOwnerEpoch request_owner_epoch;
  ActiveBundlePublicationGeneration publication_generation;
  ContentDigest active_public_pointer_digest;

  [[nodiscard]] bool locally_valid() const noexcept {
    const auto& digest = active_public_pointer_digest.value();
    return !request_owner_epoch.empty() &&
           request_owner_epoch.value().size() <= kMaxLifecycleIdentityBytes &&
           publication_generation.nonzero() && digest.size() == 71U &&
           digest.starts_with("sha256:") &&
           std::all_of(digest.begin() + 7, digest.end(), [](char c) {
             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
  }
  friend bool operator==(const CCLCLifecycleTargetRetirementScopeDTO&,
                         const CCLCLifecycleTargetRetirementScopeDTO&) = default;
};

[[nodiscard]] inline bool ValidCCLCRetirementTargets(
    const std::vector<ComponentLifecycleTargetDTO>& targets) noexcept {
  return targets.size() <= kMaximumCCLCComponentWatermarks &&
         std::is_sorted(targets.begin(), targets.end(), [](const auto& a, const auto& b) {
           return a.controllable_component_id < b.controllable_component_id;
         }) &&
         std::adjacent_find(targets.begin(), targets.end(), [](const auto& a, const auto& b) {
           return a.controllable_component_id == b.controllable_component_id;
         }) == targets.end() &&
         std::all_of(targets.begin(), targets.end(), [](const auto& target) {
           return target.locally_valid() &&
                  (!target.recovered_cause ||
                   parse_lifecycle_blocking_cause_class(to_string(*target.recovered_cause)) ==
                       target.recovered_cause);
         });
}

struct CCLCLifecycleTargetRetirementIntentDTO final {
  PublicRequestContextDTO request_context;
  CCLCLifecycleTargetRetirementScopeDTO scope;
  std::vector<ComponentLifecycleTargetDTO> expected_targets;

  [[nodiscard]] bool locally_valid() const noexcept {
    return scope.locally_valid() &&
           request_context.request_owner_epoch == scope.request_owner_epoch &&
           !request_context.request_id.empty() &&
           request_context.request_id.value().size() <= kMaxLifecycleIdentityBytes &&
           !request_context.source_label.empty() &&
           request_context.source_label.size() <= kMaxLifecycleRequestLabelBytes &&
           (!request_context.correlation_id ||
            (!request_context.correlation_id->empty() &&
             request_context.correlation_id->size() <= kMaxLifecycleRequestLabelBytes)) &&
           ValidCCLCRetirementTargets(expected_targets);
  }
  friend bool operator==(const CCLCLifecycleTargetRetirementIntentDTO&,
                         const CCLCLifecycleTargetRetirementIntentDTO&) = default;
};

struct CCLCLifecycleTargetRetirementOfferDTO final {
  CCLCLifecycleTargetRetirementScopeDTO scope;
  std::vector<ComponentLifecycleTargetDTO> targets;
  [[nodiscard]] bool locally_valid() const noexcept {
    return scope.locally_valid() && ValidCCLCRetirementTargets(targets);
  }
};

struct CCLCLifecycleTargetRetirementResultDTO final {
  RequestReplayStatusDTO replay_status;
  CCLCLifecycleTargetRetirementScopeDTO scope;
  std::vector<ComponentLifecycleTargetDTO> retired_targets;
  PublicRequestContextDTO request_context;
  [[nodiscard]] bool locally_valid() const noexcept {
    return (replay_status == RequestReplayStatusDTO::kAcceptedNew ||
            replay_status == RequestReplayStatusDTO::kReplayedExact) &&
           CCLCLifecycleTargetRetirementIntentDTO{request_context, scope, retired_targets}.locally_valid();
  }
};

/// Trusted caller distinction: receiver startup retires Active intent, while
/// an exact settled capacity checkpoint preserves the same live product.
enum class CCLCOwnerEpochTransitionPurpose {
  kRestart,
  kLiveCapacityCheckpoint,
};

/// Highest durably minted lifecycle generation for one component.
struct ComponentLifecycleGenerationWatermarkDTO final {
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;

  [[nodiscard]] bool locally_valid() const noexcept {
    return !controllable_component_id.empty() &&
           controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes &&
           component_lifecycle_generation.nonzero();
  }

  friend bool operator==(const ComponentLifecycleGenerationWatermarkDTO&,
                         const ComponentLifecycleGenerationWatermarkDTO&) = default;
};

/// Atomic readback of the journal-owned current lifecycle fact for one
/// component.
///
/// A missing watermark and state means the component has never entered the
/// CCLC journal.  A present watermark with no state means a generation was
/// durably minted while no coherent current state was available.  A present
/// state belongs exactly to
/// the returned watermark; older state is never exposed as current.
struct ComponentLifecycleCurrentFactReadbackDTO final {
  ControllableComponentId controllable_component_id;
  std::optional<ComponentLifecycleGeneration> durable_generation;
  std::optional<ComponentLifecycleStateDTO> current_state;

  [[nodiscard]] bool locally_valid() const noexcept {
    return !controllable_component_id.empty() &&
           controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes &&
           (!current_state.has_value() || durable_generation.has_value()) &&
           (!durable_generation.has_value() || durable_generation->nonzero()) &&
           (!current_state.has_value() || !to_string(*current_state).empty());
  }

  friend bool operator==(const ComponentLifecycleCurrentFactReadbackDTO&,
                         const ComponentLifecycleCurrentFactReadbackDTO&) = default;
};

/// One domain row in an atomic current-component fact readback. Absence is a
/// first-publication fact, not an unknown or sentinel configuration.
struct CCLCCurrentDomainFactReadbackDTO final {
  ControlDomainId control_domain_id;
  std::optional<ActiveControlConfigurationDTO> current_active_control_configuration;

  [[nodiscard]] bool locally_valid() const noexcept {
    return !control_domain_id.empty() &&
           control_domain_id.value().size() <= kMaxLifecycleIdentityBytes &&
           (!current_active_control_configuration.has_value() ||
            (current_active_control_configuration->locally_valid() &&
             current_active_control_configuration->control_domain_id == control_domain_id));
  }

  friend bool operator==(const CCLCCurrentDomainFactReadbackDTO&,
                         const CCLCCurrentDomainFactReadbackDTO&) = default;
};

/// One database-snapshot read of every requested component lifecycle fact and
/// current domain configuration. Rows are canonical and correspond
/// one-for-one to both query sets.
struct CCLCCurrentFactsReadbackDTO final {
  std::vector<ComponentLifecycleCurrentFactReadbackDTO> components;
  std::vector<CCLCCurrentDomainFactReadbackDTO> domains;

  [[nodiscard]] bool locally_valid() const noexcept {
    return !components.empty() && components.size() <= kMaxLifecycleFacts &&
           std::is_sorted(components.begin(), components.end(),
                          [](const auto& lhs, const auto& rhs) {
                            return lhs.controllable_component_id < rhs.controllable_component_id;
                          }) &&
           std::adjacent_find(components.begin(), components.end(),
                              [](const auto& lhs, const auto& rhs) {
                                return lhs.controllable_component_id ==
                                       rhs.controllable_component_id;
                              }) == components.end() &&
           std::all_of(components.begin(), components.end(),
                       [](const auto& component) { return component.locally_valid(); }) &&
           !domains.empty() && domains.size() <= kMaxLifecycleResolvedDomains &&
           std::is_sorted(domains.begin(), domains.end(),
                          [](const auto& lhs, const auto& rhs) {
                            return lhs.control_domain_id < rhs.control_domain_id;
                          }) &&
           std::adjacent_find(domains.begin(), domains.end(),
                              [](const auto& lhs, const auto& rhs) {
                                return lhs.control_domain_id == rhs.control_domain_id;
                              }) == domains.end() &&
           std::all_of(domains.begin(), domains.end(),
                       [](const auto& domain) { return domain.locally_valid(); });
  }

  friend bool operator==(const CCLCCurrentFactsReadbackDTO&,
                         const CCLCCurrentFactsReadbackDTO&) = default;
};

enum class CCLCEffectKindDTO : std::uint8_t {
  kBrandLifecyclePrimitive = 0,
};

enum class CCLCEffectAttemptStateDTO : std::uint8_t {
  kPrepared = 0,
  kCompleted,
};

/// Exact private typed effect-boundary record.
struct CCLCEffectAttemptDTO final {
  CCLCRequestKindDTO request_kind;
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  std::uint64_t effect_sequence;
  CCLCEffectKindDTO effect_kind;
  CCLCEffectAttemptStateDTO state;
  std::optional<BrandLifecyclePrimitiveIntentDTO> lifecycle_primitive_intent;
  std::optional<BrandLifecyclePrimitiveResultDTO> lifecycle_primitive_result;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCEffectAttemptDTO&, const CCLCEffectAttemptDTO&) = default;
};

struct CCLCTransactionEffectSequenceWatermarkDTO final {
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  std::uint64_t highest_effect_sequence;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCTransactionEffectSequenceWatermarkDTO&,
                         const CCLCTransactionEffectSequenceWatermarkDTO&) = default;
};

/// Durable state recovered before a replacement CCLC admits new work.
struct CCLCJournalRecoveryDTO final {
  std::uint64_t highest_lifecycle_transaction_generation;
  std::vector<ComponentLifecycleGenerationWatermarkDTO> component_watermarks;
  std::vector<ComponentLifecycleStatusDTO> interrupted_transactions;
  std::vector<CCLCTransactionEffectSequenceWatermarkDTO> effect_sequence_watermarks;
  std::vector<CCLCEffectAttemptDTO> unresolved_effect_attempts;
  std::uint64_t highest_route_selection_transaction_generation{};
  std::vector<ControlDomainActiveConfigurationGenerationWatermarkDTO>
      active_configuration_watermarks;
  /// Current complete published facts. Candidate generations that failed
  /// before publication remain represented only by the watermarks above.
  std::vector<ActiveControlConfigurationDTO> current_active_control_configurations;
  std::vector<RouteSelectionStatusDTO> interrupted_route_selections;
  std::vector<CCLCRouteEffectSequenceWatermarkDTO> route_effect_sequence_watermarks;
  std::vector<CCLCRouteEffectAttemptDTO> unresolved_route_effect_attempts;
  struct CellClaimRecovery final {
    CellAdmissionClaimIntentDTO intent;
    std::optional<CellAdmissionClaimDTO> claim;
    std::optional<CellAdmissionClaimReleaseDTO> release_intent;
    bool release_receipt_recorded{};
    bool transaction_accepted{};
    std::optional<ContentDigest> original_request_payload_digest{};
  };
  std::vector<CellClaimRecovery> cell_claims;
  /// Journal-minted namespace; the caller's startup-unique seed is not the
  /// public request epoch. Preserved counters prevent reuse after compaction.
  RequestOwnerEpoch request_owner_epoch{""};

  [[nodiscard]] bool locally_valid() const noexcept;
};

/// Atomic outcome of lifecycle request-ledger admission.
///
/// Accepted-new and exact-replay describe membership in the request journal:
/// they carry either owner-minted acceptance plus status, or a proven
/// no-admission rejection. The other outcomes carry neither. Reading any
/// result never creates or retries an effect.
struct CCLCLifecycleRequestResultDTO final {
  RequestReplayStatusDTO replay_status;
  std::optional<ComponentLifecycleAcceptedDTO> acceptance;
  std::optional<ComponentLifecycleStatusDTO> status;
  /// Exact known no-admission outcome; no transaction identity was minted.
  std::optional<ErrorDTO> rejection{};
  /// An operator Recover accepted as the authorization-only recovery of an
  /// open component incident (addendum 2): no transaction was minted and no
  /// vendor primitive is dispatched.
  std::optional<ComponentIncidentRecoveryDTO> incident_recovery{};

  [[nodiscard]] bool locally_valid() const noexcept;
};

/// Least-privilege read capability for the journal-owned materialized current
/// facts consumed by verified current-snapshot composition.
///
/// This surface cannot admit a request, mint a generation, mutate a status, or
/// persist an effect.  It exists so a production snapshot source does not need
/// the write-capable CCLC journal authority.
class ICCLCCurrentFactReader {
 public:
  virtual ~ICCLCCurrentFactReader() = default;

  [[nodiscard]] virtual Result<CCLCCurrentFactsReadbackDTO> LoadCurrentSnapshotFacts(
      const std::vector<ControllableComponentId>& canonical_controllable_component_ids,
      const std::vector<ControlDomainId>& canonical_control_domain_ids) = 0;
};

/// Mechanical durable boundary for the CCLC semantic owner.
///
/// This port owns no scope resolution, lifecycle plan, primitive selection,
/// deadline policy, or final semantic result. It atomically persists owner
/// epochs, request membership, accepted values, monotonic status replacement,
/// and generation watermarks. CCLC must commit acceptance/status before the
/// corresponding external effect or public projection.
class ICCLCJournal : public ICCLCCurrentFactReader {
 public:
  /// Exact retained row, including a retired generation; absence is not Close
  /// provenance. The terminal publisher reads under the existing CCLC exclusion.
  [[nodiscard]] virtual Result<std::optional<ComponentLifecycleTargetRetentionDTO>>
  ReadLifecycleTargetRetention(const ControllableComponentId&) {
    return Result<std::optional<ComponentLifecycleTargetRetentionDTO>>::Err(
        {"cclc_target_retirement_proof_unavailable", "retained target proof is not composed", {}});
  }
  [[nodiscard]] virtual Result<std::vector<ComponentLifecycleTargetDTO>>
  LoadLifecycleTargetsForClose(const RequestOwnerEpoch&) {
    return Result<std::vector<ComponentLifecycleTargetDTO>>::Err(
        {"cclc_target_retirement_unavailable", "target retirement read is not composed", {}});
  }
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>>
  ReadLifecycleTargetRetirementResult(const CCLCLifecycleTargetRetirementIntentDTO&) {
    return Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>>::Err(
        {"cclc_target_retirement_unavailable", "target retirement result is not composed", {}});
  }
  /// Original replay precedes current owner/target/guard checks. Fresh CAS
  /// shares the existing membership transaction and Kernel admission lease.
  [[nodiscard]] virtual Result<CCLCLifecycleTargetRetirementResultDTO>
  RetireLifecycleTargetsForClose(const RequestOwnerEpoch&,
      const CCLCLifecycleTargetRetirementIntentDTO&, const ICellCloseAdmissionLease&) {
    return Result<CCLCLifecycleTargetRetirementResultDTO>::Err(
        {"cclc_target_retirement_unavailable", "target retirement is not composed", {}});
  }
  ~ICCLCJournal() override = default;

  /// An unfinished parent Apply may have reserved generations or published
  /// partial candidate routes. Fresh ordinary mutations must wait; exact
  /// request replay remains available for observation.
  [[nodiscard]] virtual Result<std::optional<NamedApplyPublicationTransactionDTO>>
  ReadUnsettledNativeApplyPlan() = 0;

  /// Mints one never-before-used receiver namespace from a startup-unique seed
  /// plus the durable epoch counter, and returns it with prior nonterminal
  /// work. The supplied seed is not a caller-selected namespace. New admission
  /// remains closed until CCLC has reconciled those transactions and the exact
  /// Kernel owner mapping. Live callers must establish quiescence first.
  /// This compatibility entry denotes startup, including when it requires
  /// an exact settled predecessor. The explicit live-checkpoint overload
  /// also rejects atomically if any transaction, claim or effect is unresolved.
  [[nodiscard]] virtual Result<CCLCJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& unique_seed, std::int64_t started_at_ns,
      std::size_t request_membership_capacity,
      const std::optional<RequestOwnerEpoch>& quiescent_predecessor = std::nullopt) = 0;

  /// Explicit purpose for the existing live capacity checkpoint only. Startup
  /// keeps the four-argument call even when it requires a settled predecessor.
  /// The fallback preserves existing fake journal implementations.
  [[nodiscard]] virtual Result<CCLCJournalRecoveryDTO> BeginOwnerEpoch(
      const RequestOwnerEpoch& unique_seed, std::int64_t started_at_ns,
      std::size_t request_membership_capacity,
      const std::optional<RequestOwnerEpoch>& quiescent_predecessor,
      CCLCOwnerEpochTransitionPurpose purpose) {
    if (purpose != CCLCOwnerEpochTransitionPurpose::kRestart &&
        purpose != CCLCOwnerEpochTransitionPurpose::kLiveCapacityCheckpoint) {
      return Result<CCLCJournalRecoveryDTO>::Err(
          {"component_lifecycle_journal_owner_epoch_invalid", "owner transition purpose is invalid", {}});
    }
    return BeginOwnerEpoch(unique_seed, started_at_ns, request_membership_capacity,
                           quiescent_predecessor);
  }

  /// Observation only: exact active owner and the journal's settled predicate.
  /// Absence of this storage capability must refuse context exchange.
  [[nodiscard]] virtual Result<void> ReadQuiescentOwner(const RequestOwnerEpoch&) {
    return Result<void>::Err(
        {"component_lifecycle_quiescence_unavailable", "journal quiescence observation is not composed", {}});
  }

  /// Classifies only the owner-wide request membership/replay promise. A
  /// null optional is a previously unseen request in the current owner epoch.
  /// Any value is a final replay/conflict/expired/stale classification. This
  /// split lets CCLC honor replay before current-state/domain-busy checks while
  /// still reserving every domain before durable accepted-new admission.
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>>
  InspectLifecycleRequest(const RequestOwnerEpoch& active_owner_epoch,
                          const ComponentLifecycleIntentDTO& intent) = 0;

  /// Read the original request's retained result even after owner replacement.
  /// Absence/compaction is unknown, never permission to submit another effect.
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadLifecycleRequestResult(const ComponentLifecycleIntentDTO&) {
    return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
        {"cclc_result_lookup_unavailable", "lifecycle result lookup is not composed", {}});
  }

  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO> RecordLifecycleRejection(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&, const ErrorDTO&,
      const std::optional<CellAdmissionClaimIntentDTO>&) {
    return Result<CCLCLifecycleRequestResultDTO>::Err(
        {"cclc_rejection_persistence_unavailable",
         "no-admission result persistence is unavailable",
         {}});
  }

  /// Atomically accepts a new request or returns its exact bounded replay
  /// disposition. `accepted_new` candidates must already be fully validated;
  /// exact duplicates ignore their newly proposed identities and return the
  /// previously committed acceptance/status.
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayLifecycle(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleAcceptedDTO& candidate_acceptance,
      const ComponentLifecycleStatusDTO& candidate_initial_status) = 0;

  /// Private follower entry. Replay precedes the actual target-generation CAS.
  /// A fresh refusal consumes only its original no-admission membership and
  /// exact acquired-claim no-effect release intent, never a lifecycle generation.
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO> AcceptOrReplayTargetLifecycle(
      const RequestOwnerEpoch&, const ComponentLifecycleIntentDTO&,
      const ComponentLifecycleAcceptedDTO&, const ComponentLifecycleStatusDTO&,
      const ComponentLifecycleTargetExpectationDTO&, const CellAdmissionClaimDTO&) {
    return Result<CCLCLifecycleRequestResultDTO>::Err(
        {"component_lifecycle_target_acceptance_unavailable",
         "target-fenced lifecycle acceptance is not composed", {}});
  }

  /// Private selected-effect ledger entries use the stable original caller
  /// digest, independently of the owner offer proof. Readback never dispatches.
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>>
  InspectSelectedComponentBringUpRequest(const RequestOwnerEpoch&,
                                         const ComponentLifecycleIntentDTO&, const ContentDigest&) {
    return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
        {"selected_bring_up_journal_unavailable", "selected bring-up ledger is not composed", {}});
  }
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadSelectedComponentBringUpRequestResult(const ComponentLifecycleIntentDTO&,
                                            const ContentDigest&) {
    return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
        {"selected_bring_up_journal_unavailable",
         "selected bring-up result lookup is not composed",
         {}});
  }
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO>
  AcceptOrReplaySelectedComponentBringUp(const RequestOwnerEpoch&,
                                         const ComponentLifecycleIntentDTO&,
                                         const ComponentLifecycleAcceptedDTO&,
                                         const ComponentLifecycleStatusDTO&, const ContentDigest&) {
    return Result<CCLCLifecycleRequestResultDTO>::Err(
        {"selected_bring_up_journal_unavailable", "selected bring-up ledger is not composed", {}});
  }

  /// Q91 private delegated component ledger. The original live CCLC owner
  /// must prove the full held parent Apply authority before calling acceptance;
  /// persisted evidence is observation-only and never grants that authority.
  /// The parent's claim stays in its existing Manager journal, not this
  /// component's independent claim table.
  [[nodiscard]] virtual Result<ApplyComponentGenerationSelectionDTO>
  ReadApplyComponentGenerationSelection(const RequestOwnerEpoch&, const ControllableComponentId&,
      const std::optional<ApplyComponentQualifiedPredecessorDTO>& = std::nullopt) {
    return Result<ApplyComponentGenerationSelectionDTO>::Err(
        {"apply_bring_up_journal_unavailable", "delegated generation lookup is not composed", {}});
  }
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>>
  InspectApplyComponentBringUpRequest(const RequestOwnerEpoch&,
      const ComponentLifecycleIntentDTO&, const ContentDigest&) {
    return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
        {"apply_bring_up_journal_unavailable", "delegated Apply ledger is not composed", {}});
  }
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadApplyComponentBringUpRequestResult(const ComponentLifecycleIntentDTO&, const ContentDigest&) {
    return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
        {"apply_bring_up_journal_unavailable", "delegated Apply lookup is not composed", {}});
  }
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO>
  AcceptOrReplayApplyComponentBringUp(const RequestOwnerEpoch&,
      const ComponentLifecycleIntentDTO&, const ComponentLifecycleAcceptedDTO&,
      const ComponentLifecycleStatusDTO&, const ContentDigest&) {
    return Result<CCLCLifecycleRequestResultDTO>::Err(
        {"apply_bring_up_journal_unavailable", "delegated Apply ledger is not composed", {}});
  }

  [[nodiscard]] virtual Result<void> RecordSelectedCellClaimAttempt(
      const RequestOwnerEpoch&, const CellAdmissionClaimIntentDTO&, const ContentDigest&) {
    return Result<void>::Err({"selected_bring_up_journal_unavailable",
                              "selected claim persistence is not composed",
                              {}});
  }
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO>
  RecordSelectedComponentBringUpRejection(const RequestOwnerEpoch&,
                                          const ComponentLifecycleIntentDTO&, const ContentDigest&,
                                          const ErrorDTO&,
                                          const std::optional<CellAdmissionClaimIntentDTO>&) {
    return Result<CCLCLifecycleRequestResultDTO>::Err(
        {"selected_bring_up_journal_unavailable",
         "selected rejection persistence is not composed",
         {}});
  }

  [[nodiscard]] virtual Result<void> RecordCellClaimAttempt(
      const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimIntentDTO& intent) {
    (void)active_owner_epoch;
    (void)intent;
    return Result<void>::Err(
        {"component_lifecycle_claim_journal_unavailable",
         "CCLC journal has no lifecycle claim persistence", {}});
  }
  [[nodiscard]] virtual Result<void> RecordCellClaimAcquired(
      const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimDTO& claim) {
    (void)active_owner_epoch;
    (void)claim;
    return Result<void>::Err(
        {"component_lifecycle_claim_journal_unavailable",
         "CCLC journal has no lifecycle claim persistence", {}});
  }
  [[nodiscard]] virtual Result<std::uint64_t>
  RecordLifecycleTerminalAndClaimRelease(
      const RequestOwnerEpoch& active_owner_epoch,
      const ComponentLifecycleStatusDTO& status,
      const CellAdmissionClaimReleaseDTO& release) {
    (void)active_owner_epoch;
    (void)status;
    (void)release;
    return Result<std::uint64_t>::Err(
        {"component_lifecycle_claim_journal_unavailable",
         "CCLC journal has no lifecycle claim persistence", {}});
  }
  [[nodiscard]] virtual Result<void> RecordCellClaimReleaseReceipt(
      const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimReleaseDTO& release,
      const CellAdmissionRecoveryReceiptDTO& receipt) {
    (void)active_owner_epoch;
    (void)release;
    (void)receipt;
    return Result<void>::Err(
        {"component_lifecycle_claim_journal_unavailable",
         "CCLC journal has no lifecycle claim persistence", {}});
  }
  [[nodiscard]] virtual Result<void> RecordUnacceptedCellClaimRelease(
      const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimReleaseDTO& release) {
    (void)active_owner_epoch;
    (void)release;
    return Result<void>::Err(
        {"component_lifecycle_claim_journal_unavailable",
         "CCLC journal has no unaccepted-claim recovery", {}});
  }
  [[nodiscard]] virtual Result<void> RecordCellClaimNotAcquired(
      const RequestOwnerEpoch& active_owner_epoch, const CellAdmissionClaimIntentDTO& intent) {
    (void)active_owner_epoch;
    (void)intent;
    return Result<void>::Err(
        {"component_lifecycle_claim_journal_unavailable",
         "CCLC journal has no acquire-attempt recovery", {}});
  }

  /// Atomically persists one exact typed effect request and its successor
  /// owner status before CCLC invokes the external owner.
  [[nodiscard]] virtual Result<std::uint64_t> RecordEffectPrepared(
      const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& prepared_attempt,
      const ComponentLifecycleStatusDTO& successor_status) = 0;

  /// Atomically completes the matching prepared record and commits the exact
  /// successor owner status. Exact repetition is idempotent; conflict closes.
  [[nodiscard]] virtual Result<std::uint64_t> RecordEffectCompleted(
      const RequestOwnerEpoch& active_owner_epoch, const CCLCEffectAttemptDTO& completed_attempt,
      const ComponentLifecycleStatusDTO& successor_status) = 0;

  /// Replaces one transaction status only when its sequence and owner-clock
  /// timestamps advance monotonically. The transaction identity, accepted
  /// intent, accepted result, and minted generations are immutable.
  [[nodiscard]] virtual Result<std::uint64_t> RecordLifecycleStatus(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleStatusDTO& status) = 0;

  /// Read-only exact retained-result lookup. A compacted terminal result is
  /// intentionally absent here; request replay still returns the membership
  /// tombstone outcome through AcceptOrReplay.
  [[nodiscard]] virtual Result<std::optional<ComponentLifecycleStatusDTO>>
  LoadRetainedLifecycleStatus(const LifecycleTransactionId& lifecycle_transaction_id) = 0;

  /// Returns one coherent watermark/state readback. This point query performs
  /// no admission, generation minting, observation, or external effect.
  [[nodiscard]] virtual Result<ComponentLifecycleCurrentFactReadbackDTO>
  LoadCurrentComponentLifecycleFact(const ControllableComponentId& controllable_component_id) = 0;

  [[nodiscard]] virtual Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  InspectRouteSelectionRequest(const RequestOwnerEpoch& active_owner_epoch,
                               const RouteSelectionIntentDTO& intent) = 0;

  /// Read the exact original route result, including a retained predecessor
  /// namespace. No admission, epoch rotation or controller call is performed.
  [[nodiscard]] virtual Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  ReadRouteSelectionRequestResult(const RouteSelectionIntentDTO&) {
    return Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Err(
        {"cclc_result_lookup_unavailable", "route result lookup is not composed", {}});
  }

  [[nodiscard]] virtual Result<CCLCRouteSelectionRequestResultDTO> RecordRouteSelectionRejection(
      const RequestOwnerEpoch&, const RouteSelectionIntentDTO&, const ErrorDTO&,
      const std::optional<CellAdmissionClaimIntentDTO>&) {
    return Result<CCLCRouteSelectionRequestResultDTO>::Err(
        {"cclc_rejection_persistence_unavailable",
         "route no-admission persistence is unavailable",
         {}});
  }

  [[nodiscard]] virtual Result<CCLCRouteSelectionRequestResultDTO> AcceptOrReplayRouteSelection(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionIntentDTO& intent,
      const RouteSelectionAcceptedDTO& candidate_acceptance,
      const RouteSelectionStatusDTO& candidate_initial_status) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordRouteEffectPrepared(
      const RequestOwnerEpoch& active_owner_epoch,
      const CCLCRouteEffectAttemptDTO& prepared_attempt,
      const RouteSelectionStatusDTO& successor_status) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordRouteEffectCompleted(
      const RequestOwnerEpoch& active_owner_epoch,
      const CCLCRouteEffectAttemptDTO& completed_attempt,
      const RouteSelectionStatusDTO& successor_status) = 0;

  [[nodiscard]] virtual Result<std::uint64_t> RecordRouteEffectCompletedAndClaimRelease(
      const RequestOwnerEpoch&, const CCLCRouteEffectAttemptDTO&, const RouteSelectionStatusDTO&,
      const CellAdmissionClaimReleaseDTO&) {
    return Result<std::uint64_t>::Err({"component_lifecycle_claim_journal_unavailable",
                                       "CCLC route terminal claim release is unavailable",
                                       {}});
  }

  [[nodiscard]] virtual Result<std::uint64_t> RecordRouteTerminalAndClaimRelease(
      const RequestOwnerEpoch&, const RouteSelectionStatusDTO&,
      const CellAdmissionClaimReleaseDTO&) {
    return Result<std::uint64_t>::Err({"component_lifecycle_claim_journal_unavailable",
                                       "CCLC route terminal claim release is unavailable",
                                       {}});
  }

  [[nodiscard]] virtual Result<std::uint64_t> RecordRouteSelectionStatus(
      const RequestOwnerEpoch& active_owner_epoch, const RouteSelectionStatusDTO& status) = 0;

  [[nodiscard]] virtual Result<std::optional<RouteSelectionStatusDTO>>
  LoadRetainedRouteSelectionStatus(
      const RouteSelectionTransactionId& route_selection_transaction_id) = 0;

  /// Reads the sole current complete publication for one control domain.
  /// Absence is truthful before the first successful publication, including
  /// after one or more candidate generations failed known-no-effect.
  [[nodiscard]] virtual Result<std::optional<ActiveControlConfigurationDTO>>
  LoadCurrentActiveControlConfiguration(const ControlDomainId& control_domain_id) = 0;
};

}  // namespace manyforge::core
