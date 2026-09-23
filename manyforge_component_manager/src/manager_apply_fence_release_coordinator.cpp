#include "manyforge_component_manager/manager_apply_fence_release_coordinator.hpp"

#include <exception>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;

[[noreturn]] void Fail(const std::string& detail) {
  throw ManagerApplyFenceReleaseCoordinatorError(detail);
}

[[nodiscard]] std::string ExceptionDetail(const std::string& prefix, const std::exception& error) {
  return prefix + ": " + error.what();
}

[[noreturn]] void FailUncertain(
    std::string detail, const bool release_invoked_in_this_call,
    const core::ConfigurationFenceStatusQueryDTO& status_query,
    std::optional<core::ConfigurationFenceReleaseDTO> durable_request = std::nullopt,
    std::optional<core::ConfigurationFenceStatusDTO> observed_status = std::nullopt,
    std::optional<core::ConfigurationFenceReleasedDTO> observed_release = std::nullopt) {
  throw ManagerApplyFenceReleaseEffectUncertainError(
      std::move(detail), release_invoked_in_this_call, status_query, std::move(durable_request),
      std::move(observed_status), std::move(observed_release));
}

[[nodiscard]] core::ConfigurationFenceStatusQueryDTO MakeStatusQuery(
    const PersistedManagerApplyFenceAcceptance& acceptance,
    const core::RuntimeEpoch& manager_runtime_epoch) {
  core::ConfigurationFenceStatusQueryDTO query{
      core::ApplyFenceOwnerDTO{acceptance.apply_id, acceptance.apply_generation},
      acceptance.accepted_fence.configuration_fence_id,
      acceptance.accepted_fence.configuration_fence_generation,
      acceptance.accepted_fence.captured_scope,
      manager_runtime_epoch,
  };
  if (!query.valid()) {
    Fail("durable fence acceptance cannot form a valid status query");
  }
  return query;
}

[[nodiscard]] std::string CompletedActiveStateMismatch(
    const core::ConfigurationFenceStatusDTO &status,
    const core::ConfigurationFenceStatusQueryDTO &query,
    const core::ProjectionMaintenanceInitializationStatusDTO &completed,
    const PersistedManagerApplyPublicationReceipt &publication) {
  const auto &evidence = status.reconciliation_evidence;
  const auto &publication_receipt = publication.publication.publication_receipt;
  if (!status.matches(query))
    return "fence identity/scope";
  if (evidence.active_projection_revision !=
      completed.target_projection_revision)
    return "active projection";
  if (evidence.active_graph_revision !=
      completed.target_projection_revision.graph_revision)
    return "active graph";
  if (evidence.active_publication_generation !=
      publication_receipt.publication_generation)
    return "publication generation";
  if (evidence.active_public_pointer_digest !=
      publication_receipt.durable_public_pointer_content_digest)
    return "public pointer digest";
  if (evidence.affected_queued_operation_count != 0U ||
      evidence.affected_active_operation_count != 0U)
    return "affected operations still present";
  return {};
}

[[nodiscard]] ManagerApplyTerminalOutcomeWriteResult
PersistTerminal(SqliteManagerApplyJournal &journal,
                const core::ProjectionMaintenanceInitializationStatusDTO &completed,
                const PersistedManagerApplyPublicationReceipt &publication,
                const PersistedManagerApplyFenceReleased &released) {
  return journal.EnsureTerminalOutcome({
      completed.apply_id,
      completed.apply_generation,
      completed.target_projection_revision,
      publication.publication_receipt_digest,
      publication.publication_receipt_digest,
      released.fence_released_digest,
      ManagerApplyTerminalOutcomeState::kSucceeded,
      std::nullopt,
  });
}

[[nodiscard]] ManagerApplyFenceReleaseResult FinishFromDurableRelease(
    SqliteManagerApplyJournal& journal, PersistedManagerApplyFenceReleaseIntent release_intent,
    PersistedManagerApplyFenceReleased released,
    const core::ProjectionMaintenanceInitializationStatusDTO& completed,
    const PersistedManagerApplyPublicationReceipt& publication,
    const ManagerApplyFenceReleasePath path) {
  auto terminal = PersistTerminal(journal, completed, publication, released);
  return {
      ManagerApplyFenceReleaseIntentWriteResult{std::move(release_intent), false},
      ManagerApplyFenceReleasedWriteResult{std::move(released), false},
      std::move(terminal),
      path,
  };
}

}  // namespace

ManagerApplyFenceReleaseEffectUncertainError::ManagerApplyFenceReleaseEffectUncertainError(
    std::string detail, const bool release_invoked_in_this_call,
    core::ConfigurationFenceStatusQueryDTO status_query,
    std::optional<core::ConfigurationFenceReleaseDTO> durable_release_request,
    std::optional<core::ConfigurationFenceStatusDTO> observed_status,
    std::optional<core::ConfigurationFenceReleasedDTO> observed_release)
    : ManagerApplyFenceReleaseCoordinatorError(std::move(detail)),
      release_invoked_in_this_call_(release_invoked_in_this_call),
      status_query_(std::move(status_query)),
      durable_release_request_(std::move(durable_release_request)),
      observed_status_(std::move(observed_status)),
      observed_release_(std::move(observed_release)) {}

bool ManagerApplyFenceReleaseEffectUncertainError::release_invoked_in_this_call() const noexcept {
  return release_invoked_in_this_call_;
}

const core::ConfigurationFenceStatusQueryDTO&
ManagerApplyFenceReleaseEffectUncertainError::status_query() const noexcept {
  return status_query_;
}

const std::optional<core::ConfigurationFenceReleaseDTO>&
ManagerApplyFenceReleaseEffectUncertainError::durable_release_request() const noexcept {
  return durable_release_request_;
}

const std::optional<core::ConfigurationFenceStatusDTO>&
ManagerApplyFenceReleaseEffectUncertainError::observed_status() const noexcept {
  return observed_status_;
}

const std::optional<core::ConfigurationFenceReleasedDTO>&
ManagerApplyFenceReleaseEffectUncertainError::observed_release() const noexcept {
  return observed_release_;
}

ManagerApplyFenceReleaseCoordinator::ManagerApplyFenceReleaseCoordinator(
    SqliteManagerApplyJournal& apply_journal, core::IManagerKernelApplyFencePort& kernel_fence,
    core::IManagerSceneProjectionMaintenancePort& scene_owner,
    core::RuntimeEpoch manager_runtime_epoch)
    : apply_journal_(apply_journal),
      kernel_fence_(kernel_fence),
      scene_owner_(scene_owner),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)) {
  if (manager_runtime_epoch_.empty()) {
    Fail("Manager runtime epoch is empty");
  }
}

ManagerApplyFenceReleaseResult ManagerApplyFenceReleaseCoordinator::ReleaseOrRecover(
    const core::ApplyId& apply_id, const core::ApplyGeneration apply_generation) {
  std::lock_guard lock(operation_mutex_);

  const auto completed = scene_owner_.ReadMaintenanceInitialization(
      {apply_id, apply_generation});
  const auto publication = apply_journal_.ReadPublicationReceipt(apply_id, apply_generation);
  const auto acceptance = apply_journal_.ReadFenceAcceptance(apply_id, apply_generation);
  if (!completed.has_value() || !completed->valid() ||
      completed->state !=
          core::ProjectionMaintenanceInitializationState::kCompleted ||
      !publication.has_value() || !acceptance.has_value() ||
      completed->publication_receipt !=
          std::optional<core::ActiveBundlePublicationReceiptDTO>{
              publication->publication.publication_receipt}) {
    Fail(
        "fence release requires exact durable acceptance, publication, and "
        "Scene completion evidence");
  }
  const auto query = MakeStatusQuery(*acceptance, manager_runtime_epoch_);

  const auto existing_intent = apply_journal_.ReadFenceReleaseIntent(apply_id, apply_generation);
  const auto existing_release = apply_journal_.ReadFenceReleased(apply_id, apply_generation);
  const auto existing_terminal = apply_journal_.ReadTerminalOutcome(apply_id, apply_generation);
  if (existing_terminal.has_value()) {
    if (!existing_intent.has_value() || !existing_release.has_value() ||
        existing_terminal->terminal_outcome.scene_completion_digest !=
            std::optional<core::ContentDigest>{
                publication->publication_receipt_digest} ||
        existing_terminal->terminal_outcome.publication_receipt_digest !=
            publication->publication_receipt_digest ||
        existing_terminal->terminal_outcome.fence_released_digest !=
            existing_release->fence_released_digest) {
      Fail("durable terminal Apply outcome contradicts its completion chain");
    }
    return {
        ManagerApplyFenceReleaseIntentWriteResult{*existing_intent, false},
        ManagerApplyFenceReleasedWriteResult{*existing_release, false},
        ManagerApplyTerminalOutcomeWriteResult{*existing_terminal, false},
        ManagerApplyFenceReleasePath::kDurableTerminalReplay,
    };
  }
  if (existing_release.has_value()) {
    if (!existing_intent.has_value()) {
      Fail("durable fence release receipt lacks its exact intent");
    }
    return FinishFromDurableRelease(apply_journal_, *existing_intent, *existing_release, *completed,
                                    *publication,
                                    ManagerApplyFenceReleasePath::kCompletedFromDurableRelease);
  }

  std::optional<core::ConfigurationFenceStatusDTO> status;
  try {
    status = kernel_fence_.QueryStatus(query);
  } catch (const std::exception& error) {
    FailUncertain(ExceptionDetail("Kernel fence status query failed", error), false, query,
                  existing_intent.has_value()
                      ? std::optional<core::ConfigurationFenceReleaseDTO>{existing_intent
                                                                              ->fence_release_intent
                                                                              .release_request}
                      : std::nullopt);
  } catch (...) {
    FailUncertain("Kernel fence status query raised a non-standard exception", false, query,
                  existing_intent.has_value()
                      ? std::optional<core::ConfigurationFenceReleaseDTO>{existing_intent
                                                                              ->fence_release_intent
                                                                              .release_request}
                      : std::nullopt);
  }

  const auto mismatch =
      CompletedActiveStateMismatch(*status, query, *completed, *publication);
  if (!mismatch.empty()) {
    FailUncertain(
        "Kernel status does not prove the exact completed active projection "
        "with zero affected operations: " +
            mismatch,
        false, query,
        existing_intent.has_value()
            ? std::optional<
                  core::ConfigurationFenceReleaseDTO>{existing_intent
                                                          ->fence_release_intent
                                                          .release_request}
            : std::nullopt,
        *status);
  }

  if (existing_intent.has_value() &&
      status->state == core::ConfigurationFenceState::kReleased) {
    if (!status->release_receipt.has_value() ||
        status->release_receipt->request != existing_intent->fence_release_intent.release_request) {
      FailUncertain(
          "Kernel released status does not contain the exact durable release "
          "receipt",
          false, query, existing_intent->fence_release_intent.release_request, *status);
    }
    std::optional<ManagerApplyFenceReleasedWriteResult> durable_release;
    try {
      durable_release.emplace(apply_journal_.EnsureFenceReleased({
          existing_intent->fence_release_intent_digest,
          *status->release_receipt,
      }));
    } catch (const std::exception& error) {
      FailUncertain(ExceptionDetail("recovered Kernel release could not be persisted", error),
                    false, query, existing_intent->fence_release_intent.release_request, *status,
                    *status->release_receipt);
    }
    auto terminal =
        PersistTerminal(apply_journal_, *completed, *publication, durable_release->record);
    return {
        ManagerApplyFenceReleaseIntentWriteResult{*existing_intent, false},
        std::move(*durable_release),
        std::move(terminal),
        ManagerApplyFenceReleasePath::kRecoveredReleasedStatus,
    };
  }

  if (status->state != core::ConfigurationFenceState::kHeld) {
    FailUncertain(
        "Kernel fence is neither exact-held nor exactly released from the "
        "durable request",
        false, query,
        existing_intent.has_value()
            ? std::optional<core::ConfigurationFenceReleaseDTO>{existing_intent
                                                                    ->fence_release_intent
                                                                    .release_request}
            : std::nullopt,
        *status);
  }

  std::optional<ManagerApplyFenceReleaseIntentWriteResult> durable_intent;
  if (existing_intent.has_value()) {
    if (existing_intent->fence_release_intent.release_request.reconciliation_evidence !=
        status->reconciliation_evidence) {
      FailUncertain(
          "held Kernel reconciliation evidence differs from the durable "
          "release request",
          false, query, existing_intent->fence_release_intent.release_request, *status);
    }
    durable_intent.emplace(ManagerApplyFenceReleaseIntentWriteResult{*existing_intent, false});
  } else {
    core::ConfigurationFenceReleaseDTO request{
        status->reconciliation_evidence,
        manager_runtime_epoch_,
    };
    if (!request.valid()) {
      FailUncertain("held Kernel status cannot form a valid release request", false, query,
                    std::nullopt, *status);
    }
    durable_intent.emplace(apply_journal_.EnsureFenceReleaseIntent({
        publication->publication_receipt_digest,
        acceptance->acceptance_digest,
        request,
    }));
  }

  std::optional<core::ConfigurationFenceReleasedDTO> released;
  try {
    released = kernel_fence_.Release(durable_intent->record.fence_release_intent.release_request);
  } catch (const std::exception& error) {
    FailUncertain(ExceptionDetail("Kernel fence Release outcome is unknown", error), true, query,
                  durable_intent->record.fence_release_intent.release_request, *status);
  } catch (...) {
    FailUncertain(
        "Kernel fence Release raised a non-standard exception; its outcome "
        "is unknown",
        true, query, durable_intent->record.fence_release_intent.release_request, *status);
  }
  if (!released->valid() ||
      released->request != durable_intent->record.fence_release_intent.release_request) {
    FailUncertain("Kernel returned an invalid or mismatched fence release receipt", true, query,
                  durable_intent->record.fence_release_intent.release_request, *status, *released);
  }

  std::optional<ManagerApplyFenceReleasedWriteResult> durable_release;
  try {
    durable_release.emplace(apply_journal_.EnsureFenceReleased({
        durable_intent->record.fence_release_intent_digest,
        *released,
    }));
  } catch (const std::exception& error) {
    FailUncertain(
        ExceptionDetail("Kernel fence was released but its receipt could not be persisted", error),
        true, query, durable_intent->record.fence_release_intent.release_request, *status,
        *released);
  }
  auto terminal =
      PersistTerminal(apply_journal_, *completed, *publication, durable_release->record);
  return {
      std::move(*durable_intent),
      std::move(*durable_release),
      std::move(terminal),
      ManagerApplyFenceReleasePath::kReleasedFromNewCall,
  };
}

}  // namespace manyforge::component_manager
