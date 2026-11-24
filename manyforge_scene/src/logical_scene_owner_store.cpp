#include "logical_scene_owner_impl.hpp"

namespace manyforge::scene::logical_scene_owner_detail {

[[noreturn]] void ThrowCellAdmissionFailure(
    const std::string_view context, const core::ErrorDTO &error) {
  const auto message = std::string(context) + ": " + error.message;
  if (error.code == "kernel_owner_unavailable") {
    throw CellAdmissionTemporarilyUnavailable(message);
  }
  throw LogicalSceneOwnerConflictError(message);
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

void ValidateBoundedIdentity(const std::string_view value,
                             const std::string_view label) {
  if (value.empty() || value.size() > kMaximumSceneStoreIdentifierBytes ||
      std::any_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte < 0x21U || byte > 0x7EU;
      })) {
    throw std::invalid_argument(std::string(label) +
                                " must be bounded non-empty printable ASCII");
  }
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

void ValidateDigest(const std::string_view value,
                    const std::string_view label) {
  if (!core::IsExactLowercaseSha256Digest(value)) {
    throw std::invalid_argument(std::string(label) +
                                " must be an exact lowercase SHA-256 digest");
  }
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

std::int64_t CheckedSafeCounter(const std::uint64_t value,
                                const std::string_view label) {
  if (value == 0U || value > kMaximumSafeIntegral) {
    throw std::invalid_argument(
        std::string(label) +
        " must fit the nonzero RFC-8785 safe-integral range");
  }
  return static_cast<std::int64_t>(value);
}

void ValidateFixedQuery(const ProjectionBootstrapPrerequisiteQueryDTO &query) {
  if (!query.valid()) {
    throw std::invalid_argument(
        "canonical-empty scene prerequisite query is invalid");
  }
  ValidateBoundedIdentity(query.cell_id.value(), "cell_id");
  ValidateDigest(query.canonical_empty_retained_bundle_id.value(),
                 "canonical_empty_retained_bundle_id");
  ValidateDigest(query.canonical_empty_graph_revision.value(),
                 "canonical_empty_graph_revision");
}

AdmittedBootstrapAuthority AuthorityFromAdmission(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  const auto &retained = admission.retained_bundle();
  const auto &bundle = retained.value();
  if (admission.api_version() !=
          deployment::kCanonicalEmptyAdmissionApiVersion ||
      admission.canonical_bytes().empty() ||
      admission.canonical_bytes().size() >
          deployment::kMaxCanonicalEmptyAdmissionBytes ||
      !deployment::verify_retained_bundle_identity(retained) ||
      retained.retained_bundle_id() != admission.retained_bundle_id() ||
      bundle.retained_bundle_id != admission.retained_bundle_id() ||
      bundle.cell_graph.cell_id != admission.cell_id() ||
      bundle.cell_graph.graph_revision != admission.graph_revision()) {
    throw std::invalid_argument(
        "scene owner requires one complete sealed canonical-empty "
        "admission");
  }
  ValidateDigest(admission.admission_digest().value(),
                 "canonical-empty admission digest");
  ProjectionBootstrapPrerequisiteQueryDTO query{
      admission.cell_id(),
      admission.retained_bundle_id(),
      admission.graph_revision(),
  };
  ValidateFixedQuery(query);
  return {
      std::move(query),
      bundle,
  };
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

/// Returns the exact Scene-owned current native binding set: the one of the
/// latest published maintenance completion, else none.
[[nodiscard]] std::vector<core::ProjectionExecutionHostBindingDTO>
ActiveExecutionHostBindings(const DurableState &state) {
  for (auto initialization = state.maintenance_initializations.rbegin();
       initialization != state.maintenance_initializations.rend();
       ++initialization) {
    if (initialization->publication_receipt.has_value()) {
      return initialization->bindings;
    }
  }
  return {};
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

std::optional<StoredSceneOutcome>
ReadOutcomeForRequest(const SceneStoreMemory &memory,
                      const RequestKey &request_key) {
  const auto found = memory.outcomes.find(request_key);
  if (found == memory.outcomes.end()) {
    return std::nullopt;
  }
  return found->second;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

struct ProjectionEvidenceState final {
  std::size_t attempt_count{};
  std::size_t result_count{};
  bool all_results_committed{true};
  bool all_results_rollback_proven{true};
  bool any_result_committed{};
  bool any_result_uncertain{};
};

// The summary of a transaction's projection results; the per-result shape
// checks ran when each result was recorded.
ProjectionEvidenceState
ReadProjectionEvidenceState(const SceneTransactionRecord &record) {
  ProjectionEvidenceState evidence;
  evidence.attempt_count = record.projection_attempt_count;
  for (const auto state : record.projection_results) {
    switch (state) {
    case core::ProjectionCommitResultStateDTO::kCommitted:
      evidence.any_result_committed = true;
      evidence.all_results_rollback_proven = false;
      break;
    case core::ProjectionCommitResultStateDTO::kRejectedRollbackProven:
      evidence.all_results_committed = false;
      break;
    case core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain:
      evidence.all_results_committed = false;
      evidence.all_results_rollback_proven = false;
      evidence.any_result_uncertain = true;
      break;
    case core::ProjectionCommitResultStateDTO::kAccepted:
      throw LogicalSceneOwnerIntegrityError(
          "stored projection result state is invalid");
    }
    ++evidence.result_count;
  }
  if (evidence.result_count == 0U) {
    evidence.all_results_committed = false;
    evidence.all_results_rollback_proven = false;
  }
  return evidence;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

LogicalSceneSnapshotDTO
DeriveSceneCandidate(const LogicalSceneSnapshotDTO &predecessor,
                     const SceneTransactionRecord &intent) {
  auto candidate = core::ApplyResolvedSceneTransaction(
      predecessor, core::SceneTransactionDTO{
                       intent.request_key,
                       {},
                       intent.transaction_sequence,
                       intent.prior,
                       intent.proposed,
                       intent.request_digest,
                       intent.resolved_operation,
                       std::nullopt,
                   });
  if (!candidate.ok() ||
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          candidate.value()) != intent.proposed.scene_revision.content_hash) {
    throw LogicalSceneOwnerIntegrityError(
        "recorded scene delta does not yield its proposed revision");
  }
  return std::move(candidate.value());
}

// The replay of the memory rows: sequences, outcomes and projection evidence
// are checked; the Scene is the settled head, or a success recorded beyond it
// derived from its delta, or the newest maintenance completion's.
SceneTransactionStoreState
ReadSceneTransactionStoreState(const SceneStoreMemory &memory,
                               const DurableState &durable_state) {
  if (!durable_state.accepted.has_value() ||
      !durable_state.finalization_receipt.has_value()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene transaction history requires finalized bootstrap");
  }
  const auto &bootstrap_projection = durable_state.accepted->target;
  const auto &bootstrap_snapshot = durable_state.accepted->snapshot;
  const auto &history = memory.history;
  std::vector<const MaintenanceInitializationState *> completed_maintenance;
  for (const auto &initialization :
       durable_state.maintenance_initializations) {
    if (initialization.publication_receipt.has_value()) {
      completed_maintenance.push_back(&initialization);
    }
  }
  if (history.compacted_maintenance_completion_count >
      completed_maintenance.size()) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene checkpoint names absent maintenance history");
  }
  ProjectionRevisionDTO replay_projection = bootstrap_projection;
  const LogicalSceneSnapshotDTO *replay_snapshot = &bootstrap_snapshot;
  if (history.checkpoint_projection.has_value()) {
    replay_projection = *history.checkpoint_projection;
    replay_snapshot = &*history.checkpoint_snapshot;
    if (replay_snapshot->cell_id != bootstrap_snapshot.cell_id ||
        replay_snapshot->revision.scene_id !=
            bootstrap_snapshot.revision.scene_id ||
        replay_snapshot->transaction_sequence >
            history.compacted_through_transaction_sequence) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene checkpoint is outside the accepted lineage");
    }
  }
  if (history.compacted_through_transaction_sequence >= kMaximumSafeIntegral ||
      history.compacted_through_owner_event_sequence >= kMaximumSafeIntegral) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene history watermarks cannot admit another record");
  }
  SceneTransactionStoreState state{
      replay_projection,
      *replay_snapshot,
      history.compacted_through_transaction_sequence + 1U,
      history.compacted_through_owner_event_sequence + 1U,
      0U,
      false,
      false,
      std::nullopt,
      std::nullopt,
  };
  std::size_t next_maintenance_completion =
      history.compacted_maintenance_completion_count;
  // The Scene of the replayed head: `head` while it is known; the revision
  // and transaction sequence always (a success the settled head supersedes
  // leaves only those).
  const LogicalSceneSnapshotDTO *head = replay_snapshot;
  core::SceneRevision head_revision = replay_snapshot->revision;
  std::uint64_t head_sequence = replay_snapshot->transaction_sequence;
  std::optional<LogicalSceneSnapshotDTO> derived;
  // Completions captured before a committed transaction precede it, and a
  // no-op revision is not crossed.
  const auto apply_maintenance_until =
      [&](const std::optional<ProjectionRevisionDTO> &expected,
          const std::uint64_t committed_sequence) {
        while (next_maintenance_completion < completed_maintenance.size()) {
          const auto &initialization =
              *completed_maintenance[next_maintenance_completion];
          const bool captured_before_commit =
              committed_sequence != 0U &&
              initialization.authoritative_snapshot.transaction_sequence <
                  committed_sequence;
          if (expected.has_value() && state.active_projection == *expected &&
              !captured_before_commit &&
              !(initialization.predecessor == state.active_projection &&
                initialization.target == state.active_projection &&
                initialization.authoritative_snapshot.transaction_sequence ==
                    head_sequence)) {
            break;
          }
          if (initialization.predecessor != state.active_projection ||
              initialization.predecessor.scene_revision != head_revision ||
              initialization.authoritative_snapshot.revision !=
                  initialization.target.scene_revision ||
              initialization.authoritative_snapshot.cell_id !=
                  bootstrap_snapshot.cell_id ||
              initialization.authoritative_snapshot.transaction_sequence !=
                  head_sequence ||
              (initialization.initial_baseline_request_digest.has_value() &&
               !initialization.initial_baseline_installed)) {
            throw LogicalSceneOwnerIntegrityError(
                "maintenance completion does not follow the authoritative "
                "Scene head");
          }
          state.active_projection = initialization.target;
          head = &initialization.authoritative_snapshot;
          head_revision = head->revision;
          ++next_maintenance_completion;
        }
        return !expected.has_value() || state.active_projection == *expected;
      };
  struct ReplayedRecord final {
    const SceneTransactionRecord *intent;
    std::optional<StoredSceneOutcome> outcome;
  };
  std::vector<ReplayedRecord> records;
  std::uint64_t expected_sequence = state.next_transaction_sequence;
  for (const auto &intent : memory.transactions) {
    if (intent.transaction_sequence <=
        history.compacted_through_transaction_sequence) {
      continue;
    }
    if (!records.empty() &&
        (!records.back().outcome.has_value() ||
         (records.back().outcome->projection_status ==
              SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
          records.back().intent->transaction_sequence >
              history.recovered_through_transaction_sequence))) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene history continues after an unresolved or "
          "desynchronized transaction");
    }
    if (intent.transaction_sequence != expected_sequence) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene transaction sequence is not contiguous");
    }
    if (intent.required_projection_count > 256U ||
        intent.proposed.retained_bundle_id != intent.prior.retained_bundle_id ||
        intent.proposed.graph_revision != intent.prior.graph_revision ||
        intent.proposed.scene_revision.scene_id !=
            intent.prior.scene_revision.scene_id ||
        intent.prior.scene_revision.scene_id !=
            bootstrap_snapshot.revision.scene_id) {
      throw LogicalSceneOwnerIntegrityError(
          "durable logical-scene transaction intent is invalid");
    }
    const auto projection_evidence = ReadProjectionEvidenceState(intent);
    const auto attempts = projection_evidence.attempt_count;
    const auto results = projection_evidence.result_count;
    if (attempts > intent.required_projection_count || results > attempts) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene projection evidence cardinality is invalid");
    }
    const auto outcome = ReadOutcomeForRequest(memory, intent.request_key);
    if (!outcome.has_value()) {
      state.unresolved_transaction_sequence = expected_sequence;
      if (attempts != 0U || results != 0U ||
          intent.required_projection_count != 0U) {
        state.projection_desynchronized = true;
      }
      state.unresolved_native_attempt = true;
    } else {
      if (outcome->transaction_sequence != expected_sequence ||
          outcome->request_digest != intent.request_digest ||
          outcome->attempted_projection_count != attempts ||
          (outcome->projection_status !=
               SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
           results != attempts)) {
        throw LogicalSceneOwnerIntegrityError(
            "logical-scene outcome does not match its durable intent");
      }
      if (outcome->success && attempts != intent.required_projection_count) {
        throw LogicalSceneOwnerIntegrityError(
            "successful scene outcome lacks complete projection evidence");
      }
      if ((outcome->success && !projection_evidence.all_results_committed &&
           attempts != 0U) ||
          (outcome->projection_status ==
               SceneProjectionCommitStatusDTO::kRejectedRollbackProven &&
           !projection_evidence.all_results_rollback_proven) ||
          (outcome->projection_status ==
               SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
           !projection_evidence.any_result_uncertain &&
           !projection_evidence.any_result_committed && results == attempts)) {
        throw LogicalSceneOwnerIntegrityError(
            "scene outcome contradicts its native projection evidence");
      }
      if (outcome->projection_status ==
          SceneProjectionCommitStatusDTO::kFailedProjectionUncertain) {
        state.latest_uncertain_transaction_sequence = expected_sequence;
        if (expected_sequence >
            history.recovered_through_transaction_sequence) {
          state.projection_desynchronized = true;
        }
      }
    }
    records.push_back(ReplayedRecord{&intent, outcome});
    ++expected_sequence;
    state.next_transaction_sequence = expected_sequence;
  }
  state.resident_transaction_count = records.size();
  if (records.size() > kMaximumLogicalSceneTransactions) {
    throw LogicalSceneOwnerLimitError(
        "logical-scene transaction ledger exceeds its fixed bound");
  }
  if (history.recovered_through_transaction_sequence >=
      state.next_transaction_sequence) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene recovery watermark exceeds durable transaction "
        "history");
  }

  std::vector<bool> transaction_event_seen(records.size(), false);
  std::uint64_t expected_event_sequence = state.next_owner_event_sequence;
  std::size_t resident_event_count = 0U;
  bool history_sealed = false;
  for (const auto &event : memory.owner_events) {
    if (event.owner_event_sequence <=
        history.compacted_through_owner_event_sequence) {
      continue;
    }
    if (history_sealed) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene owner history continues after an unresolved or "
          "desynchronized event");
    }
    if (++resident_event_count > kMaximumLogicalSceneOwnerEvents ||
        expected_event_sequence > kMaximumSafeIntegral ||
        event.owner_event_sequence != expected_event_sequence) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene owner event sequence is invalid");
    }
    const auto sequence = event.transaction_sequence;
    if (sequence == 0U ||
        sequence <= history.compacted_through_transaction_sequence ||
        sequence >= expected_sequence) {
      throw LogicalSceneOwnerIntegrityError(
          "scene transaction owner event references an absent intent");
    }
    const auto index = static_cast<std::size_t>(
        sequence - history.compacted_through_transaction_sequence - 1U);
    if (index >= records.size()) {
      throw LogicalSceneOwnerIntegrityError(
          "scene transaction owner event exceeds resident history");
    }
    auto &record = records[index];
    const std::uint64_t committed_sequence =
        record.outcome.has_value() && record.outcome->success
            ? record.intent->transaction_sequence
            : 0U;
    if (transaction_event_seen[index] ||
        !apply_maintenance_until(record.intent->prior, committed_sequence)) {
      throw LogicalSceneOwnerIntegrityError(
          "scene transaction event does not follow the active composite");
    }
    transaction_event_seen[index] = true;
    if (!record.outcome.has_value()) {
      history_sealed = true;
    } else if (record.outcome->success) {
      const auto &intent = *record.intent;
      const auto &settled = memory.settled_head;
      if (settled.has_value() &&
          intent.transaction_sequence == settled->transaction_sequence) {
        if (settled->snapshot.revision != intent.proposed.scene_revision) {
          throw LogicalSceneOwnerIntegrityError(
              "settled Scene head differs from its transaction");
        }
        head = &settled->snapshot;
      } else if (settled.has_value() &&
                 intent.transaction_sequence < settled->transaction_sequence) {
        head = nullptr;
      } else {
        if (head == nullptr) {
          throw LogicalSceneOwnerIntegrityError(
              "recorded scene success has no known predecessor");
        }
        auto candidate = DeriveSceneCandidate(*head, intent);
        derived = std::move(candidate);
        head = &*derived;
      }
      state.active_projection = intent.proposed;
      head_revision = intent.proposed.scene_revision;
      head_sequence = intent.transaction_sequence;
    } else if (record.outcome->projection_status ==
                   SceneProjectionCommitStatusDTO::kFailedProjectionUncertain &&
               record.intent->transaction_sequence >
                   history.recovered_through_transaction_sequence) {
      history_sealed = true;
    }
    ++expected_event_sequence;
  }
  state.next_owner_event_sequence = expected_event_sequence;

  for (std::size_t index = 0U; index < records.size(); ++index) {
    if (!transaction_event_seen[index]) {
      throw LogicalSceneOwnerIntegrityError(
          "logical-scene intent lacks its owner-order event");
    }
  }
  static_cast<void>(apply_maintenance_until(std::nullopt, 0U));
  if (head == nullptr) {
    throw LogicalSceneOwnerIntegrityError(
        "replayed Scene head is not the settled head");
  }
  if (head != replay_snapshot) {
    state.active_snapshot = *head;
  }
  if (!durable_state.active_projection.has_value() ||
      durable_state.active_projection->retained_bundle_id !=
          state.active_projection.retained_bundle_id ||
      durable_state.active_projection->graph_revision !=
          state.active_projection.graph_revision ||
      durable_state.active_projection->scene_revision.scene_id !=
          state.active_projection.scene_revision.scene_id ||
      durable_state.active_projection->scene_revision.revision_id >
          state.active_projection.scene_revision.revision_id ||
      (durable_state.active_projection->scene_revision.revision_id ==
           state.active_projection.scene_revision.revision_id &&
       durable_state.active_projection->scene_revision.content_hash !=
           state.active_projection.scene_revision.content_hash)) {
    throw LogicalSceneOwnerIntegrityError(
        "typed structural head disagrees with owner-event replay");
  }
  return state;
}

void InsertSceneOutcome(SceneStoreMemory &memory,
                        const RequestKey &request_key,
                        const ContentDigest &request_digest,
                        const std::optional<std::uint64_t> transaction_sequence,
                        const bool success,
                        const SceneProjectionCommitStatusDTO projection_status,
                        const std::size_t attempted_projection_count,
                        const std::optional<ErrorDTO> &error) {
  memory.outcomes.emplace(
      request_key,
      StoredSceneOutcome{request_key, request_digest, transaction_sequence,
                         success, projection_status,
                         attempted_projection_count, error});
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

SceneCommitResultDTO
ResultFromStoredOutcome(const SceneStoreMemory &memory,
                        const StoredSceneOutcome &outcome,
                        const core::RequestReplayStatusDTO replay_status) {
  if (!outcome.success) {
    return FailedSceneResult(outcome.request_key, replay_status, *outcome.error,
                             outcome.projection_status);
  }
  const auto sequence = *outcome.transaction_sequence;
  const auto intent = std::find_if(
      memory.transactions.begin(), memory.transactions.end(),
      [&](const SceneTransactionRecord &record) {
        return record.transaction_sequence == sequence;
      });
  if (intent == memory.transactions.end()) {
    throw LogicalSceneOwnerIntegrityError(
        "scene result references an absent transaction intent");
  }
  SceneCommitResultDTO result{
      outcome.request_key,
      replay_status,
      true,
      std::nullopt,
      sequence,
      intent->proposed,
      SceneProjectionCommitStatusDTO::kCommitted,
      intent->update_status,
  };
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "stored scene outcome cannot form a valid public result");
  }
  return result;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

// The columns of the finalization row a Finalize records.
struct FinalizationColumns final {
  std::string bootstrap_id;
  std::int64_t bootstrap_generation;
  std::int64_t publication_generation;
  std::string publication_bootstrap_id;
  std::int64_t publication_bootstrap_generation;
  std::string candidate_retained_bundle_id;
  std::string candidate_graph_revision;
  std::string candidate_scene_id;
  std::int64_t candidate_scene_revision_id;
  std::string candidate_scene_content_hash;
  std::string request_retained_bundle_id;
  std::string request_graph_revision;
  std::string private_materialization_digest;
  std::string compatibility_materialization_digest;
  std::string public_pointer_digest;
};

// The receipt the finalization row stands for, checked against its
// acceptance.
ActiveBundlePublicationReceiptDTO
FinalizationReceipt(const FinalizationColumns &row,
                    const std::optional<AcceptedState> &accepted,
                    const bool rebuilt) {
  const auto &bootstrap_id = row.bootstrap_id;
  const auto bootstrap_generation = row.bootstrap_generation;
  const auto publication_generation = row.publication_generation;
  const auto &publication_bootstrap_id = row.publication_bootstrap_id;
  const auto publication_bootstrap_generation =
      row.publication_bootstrap_generation;
  const auto &candidate_retained_bundle_id = row.candidate_retained_bundle_id;
  const auto &candidate_graph_revision = row.candidate_graph_revision;
  const auto &candidate_scene_id = row.candidate_scene_id;
  const auto candidate_scene_revision_id = row.candidate_scene_revision_id;
  const auto &candidate_scene_content_hash = row.candidate_scene_content_hash;
  const auto &request_retained_bundle_id = row.request_retained_bundle_id;
  const auto &request_graph_revision = row.request_graph_revision;
  const auto &private_materialization_digest =
      row.private_materialization_digest;
  const auto &compatibility_materialization_digest =
      row.compatibility_materialization_digest;
  const auto &public_pointer_digest = row.public_pointer_digest;
  if (!accepted.has_value() || !rebuilt || publication_generation != 1 ||
      bootstrap_id != accepted->bootstrap_id.value() ||
      publication_bootstrap_id != bootstrap_id ||
      bootstrap_generation !=
          static_cast<std::int64_t>(accepted->bootstrap_generation.value()) ||
      publication_bootstrap_generation != bootstrap_generation ||
      candidate_retained_bundle_id !=
          accepted->target.retained_bundle_id.value() ||
      candidate_graph_revision != accepted->target.graph_revision.value() ||
      candidate_scene_id != accepted->target.scene_revision.scene_id.value() ||
      candidate_scene_revision_id !=
          static_cast<std::int64_t>(
              accepted->target.scene_revision.revision_id) ||
      candidate_scene_content_hash !=
          accepted->target.scene_revision.content_hash.value() ||
      request_retained_bundle_id != candidate_retained_bundle_id ||
      request_graph_revision != candidate_graph_revision) {
    throw LogicalSceneOwnerIntegrityError(
        "stored finalization receipt does not match its accepted target");
  }
  ValidateDigest(private_materialization_digest,
                 "stored private materialization digest");
  ValidateDigest(compatibility_materialization_digest,
                 "stored compatibility materialization digest");
  ValidateDigest(public_pointer_digest, "stored public-pointer digest");

  ActiveBundlePublicationRequestDTO request{
      BootstrapPublicationTransactionDTO{
          BootstrapId(publication_bootstrap_id),
          BootstrapGeneration(
              static_cast<std::uint64_t>(publication_bootstrap_generation)),
      },
      accepted->target,
      RetainedBundleId(request_retained_bundle_id),
      GraphRevision(request_graph_revision),
      std::nullopt,
      ContentDigest(private_materialization_digest),
  };
  ActiveBundlePublicationReceiptDTO receipt{
      std::move(request),
      ActiveBundlePublicationGeneration(
          static_cast<std::uint64_t>(publication_generation)),
      ContentDigest(public_pointer_digest),
  };
  if (!receipt.matches_bootstrap(accepted->bootstrap_id,
                                 accepted->bootstrap_generation,
                                 accepted->target)) {
    throw LogicalSceneOwnerIntegrityError(
        "stored finalization receipt is structurally invalid");
  }
  return receipt;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

DurableState ReadDurableState(const SceneStoreMemory &memory) {
  DurableState state;
  state.accepted = memory.accepted;
  state.rebuilt = memory.rebuilt;
  state.finalization_receipt = memory.finalization_receipt;
  state.maintenance_initializations = memory.maintenance_initializations;
  if (state.accepted.has_value() && state.finalization_receipt.has_value()) {
    state.active_projection = state.accepted->target;
    state.active_scene_snapshot = state.accepted->snapshot;
    for (const auto &initialization : state.maintenance_initializations) {
      if (initialization.publication_receipt.has_value()) {
        state.active_projection = initialization.target;
      }
    }
  }
  return state;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

DurableState ReadCompleteDurableState(const SceneStoreMemory &memory) {
  auto state = ReadDurableState(memory);
  if (!state.finalization_receipt.has_value()) {
    return state;
  }
  const auto transactions = ReadSceneTransactionStoreState(memory, state);
  state.active_projection = transactions.active_projection;
  state.active_scene_snapshot = transactions.active_snapshot;
  for (const auto &initialization : state.maintenance_initializations) {
    if (initialization.publication_receipt.has_value()) {
      state.active_projection = ProjectionRevisionDTO{
          initialization.target.retained_bundle_id,
          initialization.target.graph_revision,
          state.active_scene_snapshot->revision};
    }
  }
  state.projection_desynchronized = transactions.projection_desynchronized;
  state.unresolved_scene_transaction = transactions.unresolved_native_attempt;
  state.latest_uncertain_transaction_sequence =
      transactions.latest_uncertain_transaction_sequence;
  return state;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

void ValidateCanonicalEmptyIntent(
    const ProjectionBootstrapIntentDTO &intent,
    const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query,
    const RetainedBundleDTO &admitted_bundle) {
  // Check cardinality before the DTO's canonical-binding loop. A retained
  // execution-host process is infrastructure, not a native scene projection;
  // the structurally empty predecessor still has zero native bindings.
  if (!intent.execution_host_bindings.empty()) {
    throw LogicalSceneOwnerConflictError(
        "canonical-empty owner does not implement nonzero native bindings");
  }
  if (!intent.valid()) {
    throw std::invalid_argument("projection bootstrap intent is invalid");
  }
  ValidateBoundedIdentity(intent.bootstrap_id.value(), "bootstrap_id");
  static_cast<void>(CheckedSafeCounter(intent.bootstrap_generation.value(),
                                       "bootstrap_generation"));
  const auto &bundle = intent.canonical_empty_retained_bundle;
  const auto &graph = bundle.cell_graph;
  if (intent.cell_id != fixed_query.cell_id ||
      bundle.api_version != core::kRetainedBundleApiVersion ||
      bundle.retained_bundle_id !=
          fixed_query.canonical_empty_retained_bundle_id ||
      bundle != admitted_bundle ||
      graph.api_version != "manyforge.cell_graph.v1" ||
      graph.cell_id != fixed_query.cell_id ||
      graph.graph_revision != fixed_query.canonical_empty_graph_revision ||
      graph.runtime_units.size() > 1U || !graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty() ||
      !bundle.desired_control_configurations.empty() ||
      !bundle.capability_provider_bindings.empty() ||
      !bundle.compiled_products.viewer_model.has_value() ||
      !bundle.compiled_products.control_domain_products.empty() ||
      !bundle.compiled_products.symbol_role_maps.empty() ||
      bundle.compiled_products.planning_collision_model.product_id.empty() ||
      bundle.compiled_products.planning_collision_model.product_kind !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      bundle.compiled_products.planning_collision_model.product_contract_id
          .empty()) {
    throw LogicalSceneOwnerConflictError(
        "bootstrap intent does not match the admitted canonical-empty "
        "cell/bundle/graph envelope");
  }
  if (graph.runtime_units.empty() !=
      bundle.compiled_products.runtime_unit_products.empty()) {
    throw LogicalSceneOwnerConflictError(
        "canonical-empty execution host declaration and product disagree");
  }
  if (!graph.runtime_units.empty()) {
    const auto &runtime = graph.runtime_units.front();
    const auto &products =
        bundle.compiled_products.runtime_unit_products.front();
    if (!runtime.dependency_runtime_unit_ids.empty() ||
        products.runtime_unit_id != runtime.runtime_unit_id ||
        products.products.size() != 1U ||
        products.products.front().product_kind !=
            core::RetainedProductKind::kRuntimeUnitConfiguration ||
        products.products.front().product_contract_id.empty()) {
      throw LogicalSceneOwnerConflictError(
          "canonical-empty execution host envelope is incomplete");
    }
  }
  const auto &viewer_model = *bundle.compiled_products.viewer_model;
  if (viewer_model.product_id.empty() ||
      viewer_model.product_kind != core::RetainedProductKind::kViewerModel ||
      viewer_model.product_contract_id.empty()) {
    throw LogicalSceneOwnerConflictError(
        "bootstrap intent does not match the fixed canonical-empty viewer "
        "product envelope");
  }
  ValidateDigest(bundle.compiled_products.planning_collision_model
                     .product_contract_revision.value(),
                 "planning collision model contract revision");
  ValidateDigest(viewer_model.product_contract_revision.value(),
                 "viewer model contract revision");
}

AcceptedState
MintAcceptedState(const ProjectionBootstrapIntentDTO &intent,
                  const ProjectionBootstrapPrerequisiteQueryDTO &fixed_query) {
  try {
    const auto scene_id = core::logical_scene_identity::MintSceneId(
        LogicalSceneBootstrapIdentityInputDTO{fixed_query.cell_id,
                                              intent.bootstrap_id,
                                              intent.bootstrap_generation});
    CollisionPolicySnapshot policy;
    policy.revision = {1U,
                       std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
    LogicalSceneSnapshotDTO snapshot{
        SceneRevision{
            scene_id,
            1U,
            ContentDigest("sha256:"
                          "0000000000000000000000000000000000000000000000000000"
                          "000000000000"),
        },
        fixed_query.cell_id,
        0U,
        0U,
        0U,
        std::move(policy),
        {},
        {},
    };
    snapshot.revision.content_hash =
        core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot);
    ProjectionRevisionDTO target{
        fixed_query.canonical_empty_retained_bundle_id,
        fixed_query.canonical_empty_graph_revision,
        snapshot.revision,
    };
    if (!snapshot.canonical_empty_for(
            {target.scene_revision, fixed_query.cell_id}) ||
        !target.valid()) {
      throw LogicalSceneOwnerIntegrityError(
          "qualified codec did not produce an exact canonical-empty target");
    }
    return {
        intent.bootstrap_id,
        intent.bootstrap_generation,
        std::move(target),
        std::move(snapshot),
    };
  } catch (const LogicalSceneOwnerError &) {
    throw;
  } catch (const std::exception &error) {
    throw LogicalSceneOwnerIntegrityError(
        "cannot mint canonical-empty logical-scene identity: " +
        std::string(error.what()));
  }
}

void ValidateFinalizeRequest(const ProjectionBootstrapFinalizeDTO &request,
                             const AcceptedState &accepted) {
  if (!request.valid() || request.bootstrap_id != accepted.bootstrap_id ||
      request.bootstrap_generation != accepted.bootstrap_generation ||
      request.canonical_empty_target != accepted.target ||
      request.publication_receipt.publication_generation !=
          ActiveBundlePublicationGeneration(1U) ||
      request.publication_receipt.request
          .expected_previous_publication_generation.has_value() ||
      !request.publication_receipt.matches_bootstrap(
          accepted.bootstrap_id, accepted.bootstrap_generation,
          accepted.target)) {
    throw LogicalSceneOwnerConflictError(
        "Finalize does not carry the exact generation-one canonical-empty "
        "publication receipt");
  }
  ValidateDigest(request.publication_receipt.request
                     .private_materialization_content_digest.value(),
                 "private materialization content digest");
  ValidateDigest(
      request.publication_receipt.durable_public_pointer_content_digest.value(),
      "durable public-pointer content digest");
}

// The finalization row a Finalize request records.
FinalizationColumns
FinalizationRow(const ProjectionBootstrapFinalizeDTO &finalize) {
  const auto *transaction = std::get_if<BootstrapPublicationTransactionDTO>(
      &finalize.publication_receipt.request.transaction_ref);
  if (transaction == nullptr) {
    throw LogicalSceneOwnerConflictError(
        "canonical-empty Finalize receipt is not bootstrap-tagged");
  }
  const auto &receipt = finalize.publication_receipt;
  const auto &publication_request = receipt.request;
  const auto &target = publication_request.candidate_projection_revision;
  return {
      finalize.bootstrap_id.value(),
      CheckedSafeCounter(finalize.bootstrap_generation.value(),
                         "bootstrap_generation"),
      CheckedSafeCounter(receipt.publication_generation.value(),
                         "publication_generation"),
      transaction->bootstrap_id.value(),
      CheckedSafeCounter(transaction->bootstrap_generation.value(),
                         "publication bootstrap_generation"),
      target.retained_bundle_id.value(),
      target.graph_revision.value(),
      target.scene_revision.scene_id.value(),
      static_cast<std::int64_t>(target.scene_revision.revision_id),
      target.scene_revision.content_hash.value(),
      publication_request.candidate_retained_bundle_id.value(),
      publication_request.candidate_graph_revision.value(),
      publication_request.private_materialization_content_digest.value(),
      publication_request.private_materialization_content_digest.value(),
      receipt.durable_public_pointer_content_digest.value(),
  };
}

ProjectionAggregateHealthDTO NoProjectionHealth(const AcceptedState &accepted) {
  return {
      BootstrapProjectionSetSourceDTO{
          accepted.bootstrap_id,
          accepted.bootstrap_generation,
          accepted.target,
      },
      ProjectionHealthState::kHealthy,
      {},
      std::nullopt,
  };
}

ProjectionBootstrapEvidenceDTO
NoProjectionEvidence(const AcceptedState &accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.target,
      {},
      NoProjectionHealth(accepted),
  };
}

ProjectionBootstrapAcceptedDTO AcceptedResult(const AcceptedState &accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.snapshot.cell_id,
      accepted.target,
      LogicalSceneSnapshotRoot(
          std::make_shared<const LogicalSceneSnapshotDTO>(accepted.snapshot)),
  };
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

CanonicalEmptySceneAccessStatus AccessStatus(const DurableState &state) {
  const bool has_active = state.active_projection.has_value();
  const auto *maintenance = state.LatestMaintenanceInitialization();
  // The newest row is open while it has no receipt and no result other than
  // `initialized`.
  const bool maintenance_open =
      maintenance != nullptr && !maintenance->publication_receipt.has_value() &&
      (!maintenance->result_state.has_value() ||
       *maintenance->result_state ==
           ProjectionMaintenanceInitializationState::kInitialized);
  const bool transition_open = maintenance_open;
  const bool projection_available = !state.projection_desynchronized &&
                                    !state.unresolved_scene_transaction;
  // An ended newest row takes no part: the phase is the one before it.
  const bool any_completed = std::ranges::any_of(
      state.maintenance_initializations,
      [](const MaintenanceInitializationState &row) {
        return row.publication_receipt.has_value();
      });
  return {
      maintenance == nullptr
          ? state.phase()
          : (maintenance_open
                 ? CanonicalEmptySceneBootstrapPhase::kMaintenanceInitializing
                 : (any_completed
                        ? CanonicalEmptySceneBootstrapPhase::
                              kMaintenanceCompleted
                        : state.phase())),
      has_active && !transition_open && projection_available,
      has_active && !transition_open && projection_available,
      state.active_projection,
  };
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {
namespace {

ProjectionBootstrapStatusDTO
StatusResult(const DurableState &state,
             const ProjectionBootstrapStatusQueryDTO &query) {
  if (!state.accepted.has_value() ||
      query.bootstrap_id != state.accepted->bootstrap_id ||
      query.bootstrap_generation != state.accepted->bootstrap_generation) {
    return {
        query.bootstrap_id,
        query.bootstrap_generation,
        ProjectionBootstrapStatusState::kAbsent,
        std::nullopt,
        {},
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
  }
  const auto &accepted = *state.accepted;
  if (state.finalization_receipt.has_value()) {
    return {
        accepted.bootstrap_id,
        accepted.bootstrap_generation,
        ProjectionBootstrapStatusState::kFinalized,
        accepted.target,
        {},
        NoProjectionEvidence(accepted),
        state.finalization_receipt,
        NoProjectionHealth(accepted),
        std::nullopt,
    };
  }
  if (state.rebuilt) {
    return {
        accepted.bootstrap_id,
        accepted.bootstrap_generation,
        ProjectionBootstrapStatusState::kReadyToPublish,
        accepted.target,
        {},
        NoProjectionEvidence(accepted),
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
  }
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      ProjectionBootstrapStatusState::kAccepted,
      accepted.target,
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
}

ProjectionBootstrapFinalizeResultDTO
FailedFinalizeResult(const ProjectionBootstrapFinalizeDTO &request,
                     const AcceptedState &accepted,
                     const ProjectionBootstrapFinalizeResultState state,
                     std::string code, std::string message) {
  ProjectionBootstrapFinalizeResultDTO result{
      request.bootstrap_id,
      request.bootstrap_generation,
      request.canonical_empty_target,
      request.publication_receipt,
      state,
      NoProjectionEvidence(accepted),
      std::nullopt,
      ErrorDTO{std::move(code), std::move(message), {}},
  };
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "cannot represent failed canonical-empty Finalize result");
  }
  return result;
}

} // namespace
} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene::logical_scene_owner_detail {

[[nodiscard]] const SceneObjectSnapshotDTO *
FindSceneObject(const LogicalSceneSnapshotDTO &snapshot,
                const core::SceneResourceId &scene_resource_id) {
  const auto found = std::lower_bound(
      snapshot.objects.begin(), snapshot.objects.end(), scene_resource_id,
      [](const SceneObjectSnapshotDTO &object,
         const core::SceneResourceId &id) {
        return object.scene_resource_id < id;
      });
  return found == snapshot.objects.end() ||
                 found->scene_resource_id != scene_resource_id
             ? nullptr
             : &*found;
}

} // namespace manyforge::scene::logical_scene_owner_detail

namespace manyforge::scene {

ProjectionBootstrapPrerequisiteStatusDTO
LogicalSceneOwner::ObserveProjectionBootstrapPrerequisites(
    const ProjectionBootstrapPrerequisiteQueryDTO &query) const {
  if (!query.valid()) {
    throw std::invalid_argument(
        "bootstrap prerequisite observation query is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  const auto state = impl_->ReadStateTransaction();
  const auto access = AccessStatus(state);
  if (query != impl_->fixed_query_) {
    return {
        query,
        ProjectionBootstrapPrerequisiteState::kUnavailable,
        !access.scene_writes_open,
        !access.projection_leases_open,
        std::nullopt,
        ErrorDTO{
            "projection_bootstrap_prerequisite_mismatch",
            "query does not match this scene owner's fixed admitted cell",
            {},
        },
    };
  }
  if (access.active_projection.has_value()) {
    return {
        query,
        ProjectionBootstrapPrerequisiteState::kConflictingActiveProjection,
        !access.scene_writes_open,
        !access.projection_leases_open,
        access.active_projection,
        ErrorDTO{
            "conflicting_active_projection",
            "the canonical-empty scene has already been finalized",
            {},
        },
    };
  }
  ProjectionBootstrapPrerequisiteStatusDTO ready{
      query,        ProjectionBootstrapPrerequisiteState::kReadyCommandFenced,
      true,         true,
      std::nullopt, std::nullopt,
  };
  if (!ready.ready_for(query)) {
    throw LogicalSceneOwnerIntegrityError(
        "cannot represent command-fenced bootstrap prerequisites");
  }
  return ready;
}

ProjectionBootstrapAcceptedDTO
LogicalSceneOwner::BeginProjectionBootstrap(
    const ProjectionBootstrapIntentDTO &intent) {
  ValidateCanonicalEmptyIntent(intent, impl_->fixed_query_,
                               impl_->admitted_bundle_);
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  const auto state = ReadCompleteDurableState(impl_->memory_);
  if (state.accepted.has_value()) {
    if (state.accepted->bootstrap_id != intent.bootstrap_id ||
        state.accepted->bootstrap_generation != intent.bootstrap_generation) {
      throw LogicalSceneOwnerConflictError(
          "a different canonical-empty bootstrap already owns this scene "
          "lineage");
    }
    return AcceptedResult(*state.accepted);
  }
  if (state.phase() !=
          CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced ||
      AccessStatus(state).scene_writes_open ||
      AccessStatus(state).projection_leases_open ||
      AccessStatus(state).active_projection.has_value()) {
    throw LogicalSceneOwnerConflictError(
        "Begin atomic prerequisite recheck is not command-fenced empty");
  }
  const auto accepted = MintAcceptedState(intent, impl_->fixed_query_);
  impl_->memory_.accepted = accepted;
  return AcceptedResult(accepted);
}

ProjectionBootstrapRebuildResultDTO
LogicalSceneOwner::RebuildProjectionBootstrap(
    const ProjectionBootstrapRebuildRequestDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("bootstrap Rebuild request is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  const auto state = ReadCompleteDurableState(impl_->memory_);
  if (!state.accepted.has_value() ||
      request.bootstrap_id != state.accepted->bootstrap_id ||
      request.bootstrap_generation != state.accepted->bootstrap_generation ||
      request.canonical_empty_target != state.accepted->target) {
    throw LogicalSceneOwnerConflictError(
        "Rebuild does not match the exact durable Begin acceptance");
  }
  const auto evidence = NoProjectionEvidence(*state.accepted);
  ProjectionBootstrapRebuildResultDTO result{
      state.accepted->bootstrap_id,
      state.accepted->bootstrap_generation,
      state.accepted->target,
      ProjectionBootstrapRebuildResultState::kReadyToPublish,
      evidence,
      std::nullopt,
  };
  if (!result.presence_valid() || !evidence.rebuild_results.empty() ||
      !evidence.candidate_aggregate_health.native_projection_health.empty()) {
    throw LogicalSceneOwnerIntegrityError(
        "cannot represent exact healthy zero-native Rebuild evidence");
  }
  impl_->memory_.rebuilt = true;
  return result;
}

ProjectionBootstrapStatusDTO
LogicalSceneOwner::ProjectionBootstrapStatus(
    const ProjectionBootstrapStatusQueryDTO &query) const {
  if (!query.valid()) {
    throw std::invalid_argument("bootstrap status query is invalid");
  }
  ValidateBoundedIdentity(query.bootstrap_id.value(), "bootstrap_id");
  static_cast<void>(CheckedSafeCounter(query.bootstrap_generation.value(),
                                       "bootstrap_generation"));
  std::lock_guard lock(impl_->mutex_);
  auto result =
      StatusResult(impl_->ReadSceneCommitState().durable_state, query);
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "durable state cannot form a valid bootstrap status");
  }
  return result;
}

ProjectionBootstrapFinalizeResultDTO
LogicalSceneOwner::FinalizeProjectionBootstrap(
    const ProjectionBootstrapFinalizeDTO &request) {
  if (!request.valid()) {
    throw std::invalid_argument("bootstrap Finalize request is invalid");
  }
  std::lock_guard lock(impl_->mutex_);
  impl_->InvalidateSceneCommitState();
  auto state = impl_->ReadStateTransaction();
  if (!state.accepted.has_value() || !state.rebuilt) {
    throw LogicalSceneOwnerConflictError(
        "Finalize requires the exact durable zero-native Rebuild evidence");
  }
  ValidateFinalizeRequest(request, *state.accepted);
  if (state.finalization_receipt.has_value()) {
    if (*state.finalization_receipt != request.publication_receipt) {
      throw LogicalSceneOwnerConflictError(
          "Finalize replay carries a different publication receipt");
    }
    ProjectionBootstrapFinalizeResultDTO replay{
        state.accepted->bootstrap_id,
        state.accepted->bootstrap_generation,
        state.accepted->target,
        *state.finalization_receipt,
        ProjectionBootstrapFinalizeResultState::kFinalized,
        NoProjectionEvidence(*state.accepted),
        NoProjectionHealth(*state.accepted),
        std::nullopt,
    };
    if (!replay.presence_valid()) {
      throw LogicalSceneOwnerIntegrityError(
          "durable Finalize replay is invalid");
    }
    return replay;
  }

  core::Result<ActiveBundlePublicationReceiptDTO> durable = [&] {
    try {
      return impl_->publication_reader_.ReadActiveBundlePublication(
          request.publication_receipt.publication_generation);
    } catch (const std::exception &error) {
      return core::Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "active_bundle_publication_read_uncertain",
          error.what(),
          {},
      });
    } catch (...) {
      return core::Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "active_bundle_publication_read_uncertain",
          "publication reader threw a non-standard exception",
          {},
      });
    }
  }();
  if (!durable.ok()) {
    // Only the publication owner's definitive head answers may prove this
    // receipt stale.  Its production implementation returns integrity,
    // closure, and storage failures as Result errors rather than throwing;
    // treating an unknown/failed read as stale would falsely claim that an
    // authoritative current-head comparison happened.
    const auto &read_error_code = durable.error().code;
    const bool definitively_not_current =
        read_error_code == "missing_reference" ||
        read_error_code == "stale_generation" ||
        read_error_code == "publication_generation_not_current";
    const bool uncertain = !definitively_not_current;
    return FailedFinalizeResult(
        request, *state.accepted,
        uncertain
            ? ProjectionBootstrapFinalizeResultState::kFailedProjectionUncertain
            : ProjectionBootstrapFinalizeResultState::kStale,
        uncertain ? "active_bundle_publication_read_uncertain"
                  : "active_bundle_publication_not_current",
        uncertain ? "the current publication head could not be read "
                    "authoritatively"
                  : "the requested publication generation is not the current "
                    "durable head");
  }
  if (durable.value() != request.publication_receipt) {
    return FailedFinalizeResult(
        request, *state.accepted,
        ProjectionBootstrapFinalizeResultState::kStale,
        "active_bundle_publication_receipt_mismatch",
        "the independently reread current receipt is not byte-equivalent");
  }

  state = ReadCompleteDurableState(impl_->memory_);
  if (!state.accepted.has_value() || !state.rebuilt) {
    throw LogicalSceneOwnerIntegrityError(
        "durable bootstrap state changed before Finalize commit");
  }
  ValidateFinalizeRequest(request, *state.accepted);
  const bool recorded = state.finalization_receipt.has_value();
  if (!recorded) {
    // The receipt is the one the recorded row stands for.
    state.finalization_receipt = FinalizationReceipt(
        FinalizationRow(request), state.accepted, state.rebuilt);
  }
  if (*state.finalization_receipt != request.publication_receipt) {
    throw LogicalSceneOwnerIntegrityError(
        "Finalize receipt was not recorded exactly");
  }
  if (!recorded) {
    impl_->memory_.finalization_receipt = state.finalization_receipt;
  }

  ProjectionBootstrapFinalizeResultDTO result{
      state.accepted->bootstrap_id,
      state.accepted->bootstrap_generation,
      state.accepted->target,
      *state.finalization_receipt,
      ProjectionBootstrapFinalizeResultState::kFinalized,
      NoProjectionEvidence(*state.accepted),
      NoProjectionHealth(*state.accepted),
      std::nullopt,
  };
  if (!result.presence_valid()) {
    throw LogicalSceneOwnerIntegrityError(
        "durable Finalize state cannot form a valid result");
  }
  return result;
}

std::optional<SceneCommitResultDTO> LogicalSceneOwner::ReadCommitResult(
    const SceneTransactionRequestDTO &request) const {
  const ContentDigest digest(manyforge::content_identity::sha256_digest(
      logical_scene_codec_internal::EncodeRequestDigestInput(request)));
  std::lock_guard lock(impl_->mutex_);
  const auto result = ReadOutcomeForRequest(
      impl_->memory_, request.request_context.request_key);
  if (!result.has_value())
    return std::nullopt;
  if (result->request_digest != digest)
    throw LogicalSceneOwnerConflictError(
        "scene result lookup changed the exact request");
  return ResultFromStoredOutcome(impl_->memory_, *result,
                                 core::RequestReplayStatusDTO::kReplayedExact);
}

// Reports the memory rows.
CanonicalEmptySceneStoreIntegrityReport
LogicalSceneOwner::VerifyIntegrity() const {
  std::lock_guard lock(impl_->mutex_);
  const auto &memory = impl_->memory_;
  const auto state = ReadCompleteDurableState(memory);
  std::int64_t attempt_count = 0;
  std::int64_t result_count = 0;
  for (const auto &transaction : memory.transactions) {
    attempt_count +=
        static_cast<std::int64_t>(transaction.projection_attempt_count);
    result_count +=
        static_cast<std::int64_t>(transaction.projection_results.size());
  }
  CanonicalEmptySceneStoreIntegrityReport report{
      impl_->fixed_query_,
      memory.accepted.has_value() ? 1 : 0,
      memory.rebuilt ? 1 : 0,
      memory.finalization_receipt.has_value() ? 1 : 0,
      static_cast<std::int64_t>(memory.transactions.size()),
      static_cast<std::int64_t>(memory.outcomes.size()),
      attempt_count,
      result_count,
      static_cast<std::int64_t>(memory.owner_events.size()),
      state.projection_desynchronized || state.unresolved_scene_transaction,
      AccessStatus(state),
  };
  if (!report.access.valid() || report.acceptance_row_count < 0 ||
      report.acceptance_row_count > 1 || report.rebuild_row_count < 0 ||
      report.rebuild_row_count > 1 || report.finalization_row_count < 0 ||
      report.finalization_row_count > 1 ||
      report.transaction_intent_count < 0 ||
      report.transaction_intent_count >
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactions) ||
      report.request_outcome_count < 0 ||
      report.request_outcome_count > report.transaction_intent_count ||
      report.projection_attempt_count < 0 ||
      report.projection_attempt_count >
          static_cast<std::int64_t>(kMaximumLogicalSceneTransactions * 256U) ||
      report.projection_result_count < 0 ||
      report.projection_result_count > report.projection_attempt_count ||
      report.owner_event_count < 0 ||
      report.owner_event_count >
          static_cast<std::int64_t>(kMaximumLogicalSceneOwnerEvents)) {
    throw LogicalSceneOwnerIntegrityError(
        "logical-scene integrity report violates fixed row bounds");
  }
  return report;
}

} // namespace manyforge::scene
