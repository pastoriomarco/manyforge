#pragma once

// Private to the logical-scene owner sources; not installed.

#include "manyforge_scene/logical_scene_owner.hpp"

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "logical_scene_codec_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <utility>
#include <variant>

namespace manyforge::scene::logical_scene_owner_detail {

using core::ActiveBundlePublicationGeneration;

using core::ActiveBundlePublicationReceiptDTO;

using core::ActiveBundlePublicationRequestDTO;

using core::ApplyGeneration;

using core::ApplyId;

using core::BootstrapGeneration;

using core::BootstrapId;

using core::BootstrapProjectionSetSourceDTO;

using core::BootstrapPublicationTransactionDTO;

using core::CellId;

using core::CollisionPolicySnapshot;

using core::ContentDigest;

using core::ErrorDTO;

using core::GraphRevision;

using core::LogicalSceneBootstrapIdentityInputDTO;

using core::LogicalSceneSnapshotDTO;

using core::LogicalSceneSnapshotRoot;

using core::MaintenanceProjectionSetSourceDTO;

using core::NamedApplyPublicationTransactionDTO;

using core::ProjectionAggregateHealthDTO;

using core::ProjectionBootstrapAcceptedDTO;

using core::ProjectionBootstrapEvidenceDTO;

using core::ProjectionBootstrapFinalizeDTO;

using core::ProjectionBootstrapFinalizeResultDTO;

using core::ProjectionBootstrapFinalizeResultState;

using core::ProjectionBootstrapIntentDTO;

using core::ProjectionBootstrapPrerequisiteQueryDTO;

using core::ProjectionBootstrapPrerequisiteState;

using core::ProjectionBootstrapPrerequisiteStatusDTO;

using core::ProjectionBootstrapRebuildRequestDTO;

using core::ProjectionBootstrapRebuildResultDTO;

using core::ProjectionBootstrapRebuildResultState;

using core::ProjectionBootstrapStatusDTO;

using core::ProjectionBootstrapStatusQueryDTO;

using core::ProjectionBootstrapStatusState;

using core::ProjectionHealthState;

using core::ProjectionMaintenanceCompletionDTO;

using core::ProjectionMaintenanceInitializationQueryDTO;

using core::ProjectionMaintenanceInitializationRequestDTO;

using core::ProjectionMaintenanceInitializationState;

using core::ProjectionMaintenanceInitializationStatusDTO;

using core::ProjectionRevisionDTO;

using core::ProjectionStageGeneration;

using core::RequestKey;

using core::RetainedBundleDTO;

using core::RetainedBundleId;

using core::SceneCommitResultDTO;

using core::SceneId;

using core::SceneObjectSnapshotDTO;

using core::SceneObjectStateDTO;

using core::SceneProjectionCommitStatusDTO;

using core::SceneResourceId;

using core::SceneRevision;

using core::SceneTransactionRequestDTO;

using core::SceneUpdateStatusDTO;

constexpr std::uint64_t kMaximumSafeIntegral = 9007199254740991ULL;

// One owner event per resident Scene transaction intent.
constexpr std::size_t kMaximumLogicalSceneOwnerEvents =
    kMaximumLogicalSceneTransactions;

class CellAdmissionTemporarilyUnavailable final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void ThrowCellAdmissionFailure(
    const std::string_view context, const core::ErrorDTO &error);

void ValidateBoundedIdentity(const std::string_view value,
                             const std::string_view label);

std::int64_t CheckedSafeCounter(const std::uint64_t value,
                                const std::string_view label);

void ValidateFixedQuery(const ProjectionBootstrapPrerequisiteQueryDTO &query);

struct AdmittedBootstrapAuthority final {
  ProjectionBootstrapPrerequisiteQueryDTO query;
  RetainedBundleDTO retained_bundle;
};

AdmittedBootstrapAuthority AuthorityFromAdmission(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission);

struct AcceptedState final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;
  ProjectionRevisionDTO target;
  LogicalSceneSnapshotDTO snapshot;
};

struct MaintenanceInitializationState final {
  std::uint64_t initialization_sequence{};
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionRevisionDTO predecessor;
  ProjectionRevisionDTO target;
  LogicalSceneSnapshotDTO authoritative_snapshot;
  std::vector<core::ProjectionExecutionHostBindingDTO> bindings;
  std::optional<ProjectionMaintenanceInitializationState> result_state;
  std::optional<ErrorDTO> first_error;
  bool effects_unknown{};
  std::optional<ActiveBundlePublicationReceiptDTO> publication_receipt;
  std::optional<std::uint64_t> repaired_transaction_sequence;
  std::optional<ContentDigest> initial_baseline_request_digest;
  bool initial_baseline_installed{true};
  std::optional<ErrorDTO> initial_baseline_error;
  bool initial_baseline_effects_unknown{};
  // A baseline intent row exists for this initialization.
  bool initial_baseline_intent_recorded{};
};

struct DurableState final {
  std::optional<AcceptedState> accepted;
  bool rebuilt{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;
  std::optional<ProjectionRevisionDTO> active_projection;
  std::optional<LogicalSceneSnapshotDTO> active_scene_snapshot;
  std::vector<MaintenanceInitializationState> maintenance_initializations;
  bool projection_desynchronized{};
  bool unresolved_scene_transaction{};
  std::optional<std::uint64_t> latest_uncertain_transaction_sequence;

  [[nodiscard]] const MaintenanceInitializationState *
  LatestMaintenanceInitialization() const noexcept {
    return maintenance_initializations.empty()
               ? nullptr
               : &maintenance_initializations.back();
  }

  [[nodiscard]] CanonicalEmptySceneBootstrapPhase phase() const noexcept {
    if (finalization_receipt.has_value()) {
      return CanonicalEmptySceneBootstrapPhase::kFinalized;
    }
    if (rebuilt) {
      return CanonicalEmptySceneBootstrapPhase::kReadyToPublish;
    }
    if (accepted.has_value()) {
      return CanonicalEmptySceneBootstrapPhase::kAccepted;
    }
    return CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced;
  }
};

[[nodiscard]] CanonicalEmptySceneAccessStatus
AccessStatus(const DurableState &state);

/// Returns the exact Scene-owned current native binding set: the one of the
/// latest published maintenance completion, else none.
[[nodiscard]] std::vector<core::ProjectionExecutionHostBindingDTO>
ActiveExecutionHostBindings(const DurableState &state);

struct StoredSceneOutcome final {
  RequestKey request_key;
  ContentDigest request_digest;
  std::optional<std::uint64_t> transaction_sequence;
  bool success;
  SceneProjectionCommitStatusDTO projection_status;
  std::size_t attempted_projection_count;
  std::optional<ErrorDTO> error;
};

[[nodiscard]] bool IsAuthoredSceneMutation(
    const SceneTransactionRequestDTO &request);

/// Whether a request's expected projection admits the active one. The Scene
/// revision is never a precondition: any revision of the same active
/// Deployment (bundle and graph) and Scene id is accepted, and the operation
/// applies to the latest Scene (last arrival wins). Operations still check
/// their own identities (object, attachment, carrier).
[[nodiscard]] bool ExpectedProjectionAdmitsActive(
    const SceneTransactionRequestDTO &request,
    const ProjectionRevisionDTO &active);

[[nodiscard]] core::CellAdmissionClaimIntentDTO AuthoredSceneClaimIntent(
    const SceneTransactionRequestDTO &request);

void ReleaseAuthoredSceneClaim(
    core::IKernelCellAdmissionPort &admission,
    const core::CellAdmissionClaimDTO &claim,
    const ContentDigest &terminal_evidence,
    const core::CellAdmissionSettlementKindDTO settlement_kind);

void SettleAuthoredSceneClaimIfHeld(
    core::IKernelCellAdmissionPort &admission,
    const SceneTransactionRequestDTO &request,
    const ContentDigest &request_digest, const bool success,
    const SceneProjectionCommitStatusDTO projection_status);

struct SceneStoreMemory;

/// One resident intent with a recorded outcome, as its claim settlement
/// needs it. Copied under the owner lock; settled without it (OD-376).
struct RecoveredAuthoredSceneClaim final {
  SceneTransactionRequestDTO request;
  ContentDigest request_digest;
  bool success;
  SceneProjectionCommitStatusDTO projection_status;
};

[[nodiscard]] std::vector<RecoveredAuthoredSceneClaim>
RecoveredAuthoredSceneClaims(const SceneStoreMemory &memory);

void SettleRecoveredAuthoredSceneClaims(
    const std::vector<RecoveredAuthoredSceneClaim> &claims,
    core::IKernelCellAdmissionPort &admission);

struct SceneTransactionStoreState final {
  ProjectionRevisionDTO active_projection;
  LogicalSceneSnapshotDTO active_snapshot;
  std::uint64_t next_transaction_sequence;
  std::uint64_t next_owner_event_sequence;
  std::size_t resident_transaction_count;
  bool projection_desynchronized;
  bool unresolved_native_attempt;
  std::optional<std::uint64_t> unresolved_transaction_sequence;
  std::optional<std::uint64_t> latest_uncertain_transaction_sequence;
};

/// The replayed Scene state for the ordinary commit path, derived from
/// `SceneStoreMemory`. Commit keeps it current; every other mutation drops it
/// and the next read replays the memory rows again.
struct SceneCommitStateCache final {
  DurableState durable_state;
  std::optional<SceneTransactionStoreState> transaction_state;
};

struct BoundedHistoryState final {
  std::uint64_t compaction_generation{};
  std::uint64_t compacted_through_transaction_sequence{};
  std::uint64_t compacted_through_owner_event_sequence{};
  std::size_t compacted_maintenance_completion_count{};
  std::uint64_t recovered_through_transaction_sequence{};
  std::optional<ProjectionRevisionDTO> checkpoint_projection;
  std::optional<LogicalSceneSnapshotDTO> checkpoint_snapshot;
};

/// One resident Scene transaction: its intent, the count of its projection
/// attempts and the states of its projection results, in ordinal order. The
/// intent keeps the resolved operation (the delta) and, in `proposed`, the
/// candidate's content hash; it serves request replay, conflict and
/// uncertainty settlement. The settled head, not the intent, is the Scene.
struct SceneTransactionRecord final {
  std::uint64_t transaction_sequence;
  RequestKey request_key;
  ContentDigest request_digest;
  std::string request_json;
  ProjectionRevisionDTO prior;
  ProjectionRevisionDTO proposed;
  core::ResolvedSceneOperationDTO resolved_operation;
  SceneUpdateStatusDTO update_status;
  std::size_t required_projection_count;
  std::size_t projection_attempt_count;
  std::vector<core::ProjectionCommitResultStateDTO> projection_results;
};

struct SceneOwnerEventRecord final {
  std::uint64_t owner_event_sequence;
  std::uint64_t transaction_sequence;
};

/// The Scene of the newest successful transaction its Commit settled.
struct SettledSceneHead final {
  std::uint64_t transaction_sequence;
  LogicalSceneSnapshotDTO snapshot;
};

/// The store's rows, held in memory only: the authority every read and every
/// write decision uses. An owner starts with none.
struct SceneStoreMemory final {
  std::optional<AcceptedState> accepted;
  bool rebuilt{};
  std::optional<ActiveBundlePublicationReceiptDTO> finalization_receipt;
  std::vector<MaintenanceInitializationState> maintenance_initializations;
  BoundedHistoryState history;
  std::vector<SceneTransactionRecord> transactions;
  std::map<RequestKey, StoredSceneOutcome> outcomes;
  std::vector<SceneOwnerEventRecord> owner_events;
  /// Absent until a Commit settles a success, and again after a fold (the
  /// checkpoint then holds the head).
  std::optional<SettledSceneHead> settled_head;
};

std::optional<StoredSceneOutcome>
ReadOutcomeForRequest(const SceneStoreMemory &memory,
                      const RequestKey &request_key);

/// The successor of `predecessor` that `intent` recorded: its delta applied
/// (core::ApplyResolvedSceneTransaction), refused with
/// LogicalSceneOwnerIntegrityError unless the result's content hash is the
/// intent's proposed one.
[[nodiscard]] LogicalSceneSnapshotDTO
DeriveSceneCandidate(const LogicalSceneSnapshotDTO &predecessor,
                     const SceneTransactionRecord &intent);

/// The Scene state the memory rows hold: the settled head, a recorded success
/// beyond it derived from its delta (DeriveSceneCandidate).
SceneTransactionStoreState
ReadSceneTransactionStoreState(const SceneStoreMemory &memory,
                               const DurableState &durable_state);

struct PreparedSceneHistoryCompaction final {
  BoundedHistoryState history;
};

/// Checks a compaction of the resident Scene ledger behind a checkpoint of
/// the settled head. `maintenance_fold` admits an empty ledger and a closed
/// maintenance window.
[[nodiscard]] PreparedSceneHistoryCompaction PrepareSceneHistoryCompaction(
    const SceneStoreMemory &memory, const DurableState &durable_state,
    const SceneTransactionStoreState &transactions, bool maintenance_fold);

/// Applies a prepared compaction to memory.
void CompactSceneTransactionHistory(
    SceneStoreMemory &memory, PreparedSceneHistoryCompaction compaction,
    const DurableState &durable_state,
    const SceneTransactionStoreState &transactions);

ErrorDTO SceneError(const std::string_view code, const std::string_view message,
                    std::map<std::string, std::string> details = {});

SceneCommitResultDTO
FailedSceneResult(const RequestKey &request_key,
                  const core::RequestReplayStatusDTO replay, ErrorDTO error,
                  const SceneProjectionCommitStatusDTO projection_status);

bool SamePose(const core::Pose3DTO &left,
              const core::Pose3DTO &right) noexcept;

class SceneRequestRejectionError final : public core::TypedError {
public:
  SceneRequestRejectionError(std::string code, std::string message,
                             std::map<std::string, std::string> details = {})
      : core::TypedError(std::move(code), message, std::move(details)) {}
};

struct NormalizedScenePose final {
  core::Pose3DTO pose_in_cell;
  ContentDigest resolution_evidence_digest;
};

[[nodiscard]] NormalizedScenePose
NormalizeScenePose(const core::SceneTransactionFrameRefDTO &frame_ref,
                   const core::Pose3DTO &pose_in_frame,
                   const LogicalSceneSnapshotDTO &current,
                   const ProjectionRevisionDTO &expected_projection,
                   const ContentDigest &request_digest,
                   const std::optional<core::SceneFrameResolutionReceiptDTO>
                       &frame_resolution);

/// Records one outcome row.
void InsertSceneOutcome(SceneStoreMemory &memory,
                        const RequestKey &request_key,
                        const ContentDigest &request_digest,
                        const std::optional<std::uint64_t> transaction_sequence,
                        const bool success,
                        const SceneProjectionCommitStatusDTO projection_status,
                        const std::size_t attempted_projection_count,
                        const std::optional<ErrorDTO> &error);

SceneCommitResultDTO
ResultFromStoredOutcome(const SceneStoreMemory &memory,
                        const StoredSceneOutcome &outcome,
                        const core::RequestReplayStatusDTO replay_status);

struct RequiredSceneProjection final {
  std::shared_ptr<core::IPlannerSceneProjection> projection;
  /// Absent until observed: a Commit learns it from its preflight.
  std::optional<core::ProjectionHealthDTO> preflight_health;
  core::ProjectionExecutionHostBindingDTO binding;
};

/// The client of one required projection; no native call.
[[nodiscard]] std::shared_ptr<core::IPlannerSceneProjection>
ResolveProjectionClient(const core::ProjectionExecutionHostBindingDTO &binding,
                        const core::IPlannerSceneProjectionResolver *resolver);

/// Refuses health that is malformed or names another host incarnation.
void RequireProjectionHealth(const core::ProjectionHealthDTO &health,
                             const core::ProjectionExecutionHostBindingDTO &binding);

[[nodiscard]] RequiredSceneProjection ResolveRequiredProjection(
    const core::ProjectionExecutionHostBindingDTO &binding,
    const core::IPlannerSceneProjectionResolver *resolver,
    const ProjectionRevisionDTO &selected_revision);

[[nodiscard]] std::optional<core::SceneFrameResolutionReceiptDTO>
ResolveFrameAcrossProjections(
    const core::SceneFrameResolutionSelectorDTO &selector,
    const ProjectionRevisionDTO &projection_revision,
    const std::vector<RequiredSceneProjection> &projections,
    const std::string_view lease_scope);

[[nodiscard]] core::CollisionPolicySnapshot ProjectEffectivePolicy(
    const LogicalSceneSnapshotDTO &candidate,
    const std::vector<RequiredSceneProjection> &projections);

/// The one effective policy the projections answered for `candidate`, in
/// projection order; the configured policy when there is none.
[[nodiscard]] core::CollisionPolicySnapshot SelectEffectivePolicy(
    const LogicalSceneSnapshotDTO &candidate,
    std::vector<std::optional<core::Result<core::CollisionPolicySnapshot>>>
        projected);

DurableState ReadCompleteDurableState(const SceneStoreMemory &memory);

/// Deletes one initialization; a folded completion leaves the checkpoint's
/// folded-completion count.
void DeleteMaintenanceInitialization(SceneStoreMemory &memory,
                                     const MaintenanceInitializationState &row);

[[nodiscard]] const SceneObjectSnapshotDTO *
FindSceneObject(const LogicalSceneSnapshotDTO &snapshot,
                const core::SceneResourceId &scene_resource_id);

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene {
using namespace logical_scene_owner_detail;
} // namespace manyforge::scene
