#include "manyforge_component_manager/manager_named_apply_coordinator.hpp"

#include "native_projection_transition_internal.hpp"

#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <set>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::size_t kMaximumApplyIdMintAttempts = 4U;

[[nodiscard]] bool SameDeploymentRevision(
    const core::ProjectionRevisionDTO &left,
    const core::ProjectionRevisionDTO &right) noexcept {
  return left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision;
}

/// Same cell and Scene; the revision is Apply-time evidence only.
[[nodiscard]] bool SameSnapshotScene(
    const core::LogicalSceneSnapshotIdentityDTO &left,
    const core::LogicalSceneSnapshotIdentityDTO &right) noexcept {
  return left.cell_id == right.cell_id &&
         left.revision.scene_id == right.revision.scene_id;
}

// The owner a retained Apply cause names when it carried no other producer.
constexpr std::string_view kApplyProducer = "component_manager";
// The producer of a refusal the Kernel's admission port returned.
constexpr std::string_view kKernelProducer = "kernel";
constexpr std::string_view kInstalledStateUnknown = "actual_installed_state_unknown";

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerNamedApplyCoordinatorError(detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

[[noreturn]] void FailAcknowledgementUncertain(
    const std::exception &error, const std::string_view stage,
    const std::string_view producer, const std::string &message) {
  const auto cause = core::CauseOf(error, "manager_command_rejected", producer);
  std::map<std::string, std::string> details{
      {"stage", std::string(stage)}, {"cause_code", cause.error.code},
      {"cause_message", cause.error.message}, {"cause_producer", cause.producer}};
  for (const auto &[key, value] : cause.error.details)
    details.emplace("cause." + key, value);
  throw core::TypedError("apply_acknowledgement_outcome_unknown", message,
                         std::move(details), std::string(kApplyProducer));
}

/// Throw a typed cause another owner returned, keeping its code and details.
/// `message` is the text the Apply journal retains (unchanged from before).
[[noreturn]] void FailCause(const core::ErrorDTO &cause, const std::string &message,
                            const std::string_view producer = kApplyProducer) {
  throw core::TypedError(cause.code, message, cause.details, std::string(producer));
}

/// The installed component processes cannot be established exactly.
void RequireInstalledState(const bool condition, const std::string &detail) {
  if (!condition) {
    throw core::TypedError(std::string(kInstalledStateUnknown),
                           std::string(kInstalledStateUnknown) + ": " + detail);
  }
}

[[nodiscard]] bool IsTerminal(const PersistedManagerApplyStatus &status) {
  return status.terminal_outcome.has_value();
}

/// Map one durable terminal outcome to its public result state.
///
/// A terminal failure or unknown-effect operation is never reported as success
/// and never resumes. Published success settles admission; the only failure
/// allowed to settle a transferred parent claim is a known refusal retained at
/// the pre-effect `accepted` phase.
[[nodiscard]] ManagerNamedApplyResultState ResultStateFromTerminal(
    const ManagerApplyTerminalOutcomeDTO &terminal) {
  switch (terminal.state) {
  case ManagerApplyTerminalOutcomeState::kSucceeded:
    return ManagerNamedApplyResultState::kSucceeded;
  case ManagerApplyTerminalOutcomeState::kFailed:
    return ManagerNamedApplyResultState::kFailed;
  case ManagerApplyTerminalOutcomeState::kUnknownEffect:
    return ManagerNamedApplyResultState::kUnknownEffect;
  }
  return ManagerNamedApplyResultState::kUnknownEffect;
}

/// Planned maintenance states its own boundary: only `kFailedBeforeEffect`
/// proves no configuration, runtime, component or Scene change (a claim, the
/// Kernel fence and closed readiness may exist).
[[nodiscard]] ManagerApplyEffectBoundary
MaintenanceBoundary(const ManagerPlannedMaintenanceResult &result) {
  return result.outcome == ManagerPlannedMaintenanceOutcome::kFailedBeforeEffect
             ? ManagerApplyEffectBoundary::kBeforeEffect
             : ManagerApplyEffectBoundary::kUnknown;
}

/// Admission refusals that prove this Apply never had an admission effect: the
/// Kernel refused only after finding no live, released, reconciled or
/// compacted claim for this request, before durable acquisition; or the
/// Manager's channel refused before sending. Refusals that only prove "no NEW
/// claim" (`request_result_expired`, `request_id_conflict`, and the checks made
/// before that lookup: `invalid_request`, `stale_request_owner_epoch`) are not
/// listed. This is stricter than the `kBeforeEffect` contract (no
/// configuration, runtime, component or Scene change) and so conservative.
[[nodiscard]] bool IsKnownAdmissionRefusal(const std::string_view code) {
  static constexpr std::string_view kCodes[] = {
      "stale_generation", "authoring_not_ready", "request_ledger_full",
      "kernel_owner_unavailable", "installed_process_inspection_pending"};
  return std::find(std::begin(kCodes), std::end(kCodes), code) != std::end(kCodes);
}

/// The effect boundary of one admission failure. No configuration change is
/// ever made on this path, but `kBeforeEffect` is given only for a typed known
/// refusal (this Apply never had an admission effect). `persistence_outcome_unknown`, a lost or undecodable
/// reply and anything untyped may leave a claim behind and give `kUnknown`.
[[nodiscard]] ManagerApplyEffectBoundary
AdmissionRefusalBoundary(const std::exception &error) {
  const auto *typed = dynamic_cast<const core::TypedError *>(&error);
  return typed != nullptr && IsKnownAdmissionRefusal(typed->code())
             ? ManagerApplyEffectBoundary::kBeforeEffect
             : ManagerApplyEffectBoundary::kUnknown;
}

/// The effects of a failed host Apply. Only the host's typed refusals before
/// any effect (an inconsistent candidate, an enabled replaced or removed
/// component) and a host failure the host reports as `effect: none` (caught
/// before it changed anything installed) prove the candidate was never
/// installed or published (a known `failed`); any other failure stays unknown.
[[nodiscard]] ManagerApplyEffectDisposition
PreparationFailureDisposition(const std::exception &error) {
  const auto *typed = dynamic_cast<const core::TypedError *>(&error);
  if (typed == nullptr) return ManagerApplyEffectDisposition::kUnknown;
  const auto effect = typed->details().find("effect");
  return typed->code() == kScenarioCompositionRefusedCode ||
                 typed->code() == "apply_requires_inactive_component" ||
                 (effect != typed->details().end() && effect->second == "none")
             ? ManagerApplyEffectDisposition::kKnown
             : ManagerApplyEffectDisposition::kUnknown;
}

/// A cause that carries no code of its own: the phase code, produced here.
[[nodiscard]] core::TypedCause PlainCause(const std::string &code,
                                          const std::string &message) {
  return {{code, message, {}}, std::string(kApplyProducer)};
}

[[nodiscard]] std::string BoundedCode(const std::string &code) {
  return !code.empty() && code.size() <= kMaximumManagerApplyFailureCodeBytes
             ? code
             : std::string("manager_apply_failure");
}

[[nodiscard]] ManagerNamedApplyResult RetainTerminalFailure(
    SqliteManagerApplyJournal &journal,
    const PersistedManagerApplyIntent &durable_intent,
    const std::string &failure_code, const core::TypedCause &cause,
    const ManagerApplyEffectDisposition disposition,
    const bool request_replayed,
    const ManagerApplyEffectBoundary boundary = ManagerApplyEffectBoundary::kUnknown) {
  const auto status = journal.ReadStatus(
      durable_intent.intent.apply_id, durable_intent.intent.apply_generation);
  Require(status.has_value(),
          "Manager Apply disappeared while retaining terminal failure");
  const auto bounded_code = BoundedCode(failure_code);
  const auto &failure_message = cause.error.message;
  const auto bounded_message =
      !failure_message.empty() &&
              failure_message.size() <= kMaximumManagerApplyFailureMessageBytes
          ? failure_message
          : std::string("failure detail exceeded the retained message bound");
  // Details over the record's bounds are left out; the code and message stay.
  std::map<std::string, std::string> bounded_details;
  for (const auto &[key, value] : cause.error.details) {
    if (bounded_details.size() == kMaximumManagerApplyFailureCauseDetails) break;
    if (!key.empty() && key.size() <= kMaximumManagerApplyFailureCauseDetailKeyBytes &&
        value.size() <= kMaximumManagerApplyFailureCauseDetailValueBytes)
      bounded_details.emplace(key, value);
  }
  const auto bounded_producer =
      !cause.producer.empty() &&
              cause.producer.size() <= kMaximumManagerApplyFailureProducerBytes
          ? cause.producer
          : std::string(kApplyProducer);
  const ManagerApplyTerminalOutcomeDTO outcome{
      durable_intent.intent.apply_id,
      durable_intent.intent.apply_generation,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      disposition == ManagerApplyEffectDisposition::kKnown
          ? ManagerApplyTerminalOutcomeState::kFailed
          : ManagerApplyTerminalOutcomeState::kUnknownEffect,
      ManagerApplyFailureEvidenceDTO{
          durable_intent.intent_digest,
          DeriveManagerApplyProgress(*status).phase,
          bounded_code,
          bounded_message,
          disposition,
          BoundedCode(cause.error.code),
          std::move(bounded_details),
          bounded_producer,
          boundary},
  };
  Require(outcome.valid(), "Manager Apply terminal failure is invalid");
  const auto retained = journal.EnsureTerminalOutcome(outcome);
  return {durable_intent.intent.apply_id,
          durable_intent.intent.apply_generation,
          ResultStateFromTerminal(retained.record.terminal_outcome),
          request_replayed,
          retained.record.terminal_outcome};
}

/// Retain a failure whose message is this coordinator's own text.
[[nodiscard]] ManagerNamedApplyResult RetainTerminalFailure(
    SqliteManagerApplyJournal &journal,
    const PersistedManagerApplyIntent &durable_intent,
    const std::string &failure_code, const std::string &failure_message,
    const ManagerApplyEffectDisposition disposition,
    const bool request_replayed,
    const ManagerApplyEffectBoundary boundary = ManagerApplyEffectBoundary::kUnknown) {
  return RetainTerminalFailure(journal, durable_intent, failure_code,
                               PlainCause(failure_code, failure_message),
                               disposition, request_replayed, boundary);
}

/// Retain a caught failure: a typed cause keeps its own code, details and
/// producer; any other exception is the phase code with its text.
[[nodiscard]] ManagerNamedApplyResult RetainTerminalFailure(
    SqliteManagerApplyJournal &journal,
    const PersistedManagerApplyIntent &durable_intent,
    const std::string &failure_code, const std::exception &error,
    const ManagerApplyEffectDisposition disposition,
    const bool request_replayed,
    const ManagerApplyEffectBoundary boundary = ManagerApplyEffectBoundary::kUnknown) {
  return RetainTerminalFailure(
      journal, durable_intent, failure_code,
      core::CauseOf(error, failure_code, kApplyProducer), disposition,
      request_replayed, boundary);
}

[[nodiscard]] std::shared_ptr<const deployment::RetainedBundle>
LoadBundle(const deployment::IVerifiedRetainedBundleSource &source,
           const core::RetainedBundleId &bundle_id,
           const std::string_view label) {
  auto bundle = source.get_verified(bundle_id);
  Require(bundle != nullptr &&
              deployment::verify_retained_bundle_identity(*bundle) &&
              bundle->retained_bundle_id() == bundle_id,
          std::string(label) +
              " retained bundle is absent or fails independent verification");
  return bundle;
}

class ActiveComponentApplyRefusal final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] ManagerApplyFenceExpectationDTO FenceExpectation(
    const ManagerApplyIntentDTO &intent,
    const deployment::RetainedBundle &predecessor,
    const deployment::RetainedBundle &candidate,
    const ManagerActiveRuntimePlanDTO &active_plan,
    SqliteManagerApplyJournal &journal,
    core::IManagerKernelApplyFencePort &kernel_fence,
    const core::RuntimeEpoch &manager_runtime_epoch,
    IManagerSelectedRuntimeCompositionPort *runtime_composition,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const deployment::SiteFacts &site_facts) {
  if (const auto attempt =
          journal.ReadFenceAttempt(intent.apply_id, intent.apply_generation);
      attempt.has_value()) {
    const auto &fence = attempt->fence_intent;
    return {
        fence.expected_operation_resource_generation,
        fence.expected_fence_generation,
        fence.expected_active_control_configurations,
        fence.scope,
    };
  }

  auto scope = deployment::derive_apply_configuration_fence_scope(predecessor,
                                                                  candidate);
  core::ConfigurationFenceAdmissionQueryDTO query{scope, manager_runtime_epoch};
  Require(query.valid(), "derived Kernel admission query is invalid");

  auto snapshot = kernel_fence.ObserveAdmission(query);
  Require(snapshot.matches(query),
          "Kernel returned invalid or foreign admission evidence");
  Require(SameDeploymentRevision(snapshot.active_projection_revision,
                                 intent.expected_active_projection_revision) &&
              snapshot.active_public_pointer_digest ==
                  active_plan.active_publication
                      .durable_public_pointer_content_digest &&
              snapshot.execution_host_epoch.publication_generation ==
                  active_plan.active_publication.publication_generation,
          "Kernel admission evidence does not match the current durable "
          "predecessor publication");
  if (runtime_composition != nullptr) {
    const ManagerComponentProcessInventoryRequestDTO inventory_request{
        active_plan.active_publication.request.candidate_projection_revision,
        active_plan.active_publication.publication_generation,
        active_plan.active_publication.durable_public_pointer_content_digest,
        snapshot.execution_host_epoch.runtime_unit_id,
        snapshot.execution_host_epoch.runtime_epoch,
        manager_runtime_epoch,
        true};
    const auto inventory =
        runtime_composition->InspectInstalledComponentProcesses(
            inventory_request);
    if (!inventory.ok())
      FailCause(inventory.error(),
                "actual_installed_state_unknown: " + inventory.error().message +
                    "; restart the workbench container");
    RequireInstalledState(inventory.value().valid() &&
                              inventory.value().request == inventory_request,
                          "installed process inventory is foreign");
    if (inventory.value().error)
      FailCause(*inventory.value().error,
                inventory.value().error->code + ": " +
                    inventory.value().error->message +
                    "; restart the workbench container");
    // This observation runs under the held parent admission and before a
    // fence attempt, runtime convergence or Scene staging. CONTRACTS §11: a
    // replaced or removed component that is still Active with a live process
    // is disabled first (the server-side orchestration does that); one whose
    // launch processes are gone, or that is faulted, is removed or replaced
    // without a clean Disable, in any Apply.
    const auto delta =
        deployment::derive_apply_component_delta(predecessor, candidate, site_facts);
    for (const auto &change : delta.components) {
      if (change.disposition !=
              deployment::ApplyComponentDisposition::kReplace &&
          change.disposition != deployment::ApplyComponentDisposition::kRemove)
        continue;
      const auto fact = std::find_if(
          inventory.value().component_lifecycles.begin(),
          inventory.value().component_lifecycles.end(), [&](const auto &row) {
            return row.controllable_component_id ==
                   change.controllable_component_id;
          });
      RequireInstalledState(fact != inventory.value().component_lifecycles.end(),
                            "changed component lifecycle is unavailable");
      if (fact->state == core::ComponentLifecycleStateDTO::kActive &&
          !ComponentLaunchProcessesDrained(change.controllable_component_id,
                                           inventory.value().process_occurrences))
        throw ActiveComponentApplyRefusal(
            "configuration Apply cannot replace or remove Active component " +
            change.controllable_component_id.value() + "; deactivate it first");
    }
    std::set<core::ControllableComponentId> restart;
    for (const auto &row : inventory.value().process_occurrences) {
      const bool no_launch = row.process_owner_kind ==
          ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal;
      if (no_launch) {
        const auto verified = ValidateManagerPneumaticNoneProcessOwner(
            predecessor, site_facts, inventory_request, row);
        if (!verified.ok()) FailCause(verified.error(), verified.error().message);
      }
      RequireInstalledState(row.process_presence != ProcessPresenceState::kUnknown &&
                                row.installed_runtime_identity,
                            "installed process identity or presence is "
                            "unknown; restart the workbench container");
      const auto wanted = deployment::derive_component_runtime_identity(
          candidate, site_facts, row.controllable_component_id);
      if ((!no_launch && row.process_presence != ProcessPresenceState::kPresent) ||
          wanted != row.installed_runtime_identity)
        restart.insert(row.controllable_component_id);
    }
    for (const auto &component :
         candidate.value().cell_graph.controllable_components) {
      const auto wanted = deployment::derive_component_runtime_identity(
          candidate, site_facts, component.controllable_component_id);
      const auto count = std::count_if(
          inventory.value().process_occurrences.begin(),
          inventory.value().process_occurrences.end(), [&](const auto &row) {
            return row.controllable_component_id ==
                       component.controllable_component_id &&
                   (row.process_presence == ProcessPresenceState::kPresent ||
                    row.process_owner_kind == ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal) &&
                   row.installed_runtime_identity == wanted;
          });
      RequireInstalledState(count <= 1, "component has multiple current process owners");
      if (count == 0)
        restart.insert(component.controllable_component_id);
    }
    std::vector<std::shared_ptr<const deployment::RetainedBundle>>
        terminal_products;
    std::vector<const deployment::RetainedBundle *> source_products;
    const auto old_statuses = journal.ReadStatusInventory();
    for (const auto &row : inventory.value().process_occurrences) {
      if (!restart.contains(row.controllable_component_id))
        continue;
      const auto old_identity = deployment::derive_component_runtime_identity(
          predecessor, site_facts, row.controllable_component_id);
      const auto new_identity = deployment::derive_component_runtime_identity(
          candidate, site_facts, row.controllable_component_id);
      if (old_identity == row.installed_runtime_identity ||
          new_identity == row.installed_runtime_identity)
        continue;
      std::shared_ptr<const deployment::RetainedBundle> source;
      for (const auto &status : old_statuses) {
        if (!status.terminal_outcome ||
            status.terminal_outcome->terminal_outcome.succeeded())
          continue;
        const auto product = retained_bundle_source.get_verified(
            status.intent.intent.candidate_retained_bundle_id);
        if (!product ||
            !deployment::verify_retained_bundle_identity(*product) ||
            product->value().cell_graph.cell_id != intent.cell_id ||
            product->value().cell_graph.graph_revision !=
                status.intent.intent.candidate_graph_revision)
          continue;
        if (deployment::derive_component_runtime_identity(
          *product, site_facts, row.controllable_component_id) ==
            row.installed_runtime_identity) {
          source = product;
          break;
        }
      }
      RequireInstalledState(
          source != nullptr,
          "installed failed candidate lacks its exact terminal product; "
          "restart the workbench container");
      if (std::none_of(terminal_products.begin(), terminal_products.end(),
                       [&](const auto &known) {
                         return known->retained_bundle_id() ==
                                source->retained_bundle_id();
                       })) {
        source_products.push_back(source.get());
        terminal_products.push_back(std::move(source));
      }
    }
    const std::vector<core::ControllableComponentId> components(restart.begin(),
                                                                restart.end());
    const auto actual_scope = [&] {
      try {
        return deployment::derive_apply_configuration_fence_scope(
            predecessor, candidate, components, source_products);
      } catch (const deployment::ApplyAffectedScopeError &error) {
        if (!error.code().empty())
          throw core::TypedError(error.code(), error.what(), {}, std::string(kApplyProducer));
        throw;
      }
    }();
    if (actual_scope != scope) {
      query.affected_scope = actual_scope;
      const auto expanded = kernel_fence.ObserveAdmission(query);
      RequireInstalledState(expanded.matches(query) &&
                                expanded.execution_host_epoch ==
                                    snapshot.execution_host_epoch &&
                                expanded.active_projection_revision ==
                                    snapshot.active_projection_revision &&
                                expanded.active_public_pointer_digest ==
                                    snapshot.active_public_pointer_digest,
                            "original host or publication changed during "
                            "scope observation");
      snapshot = expanded;
      scope = actual_scope;
    }
  }
  if (snapshot.affected_queued_operation_count != 0U ||
      snapshot.affected_active_operation_count != 0U) {
    throw ManagerNamedApplyBusyError(
        "named Apply rejected because its affected resources are busy");
  }
  return {
      snapshot.operation_resource_generation,
      snapshot.configuration_fence_generation,
      snapshot.active_control_configurations,
      scope,
  };
}

[[nodiscard]] bool MatchesRequest(const ManagerApplyIntentDTO &intent,
                                  const ManagerNamedApplyRequestDTO &request) {
  return intent.scene_initialization == request.scene_initialization &&
         intent.request_context == request.request_context &&
         intent.expected_active_projection_revision.retained_bundle_id ==
             request.expected_current_retained_bundle_id &&
         intent.expected_active_projection_revision.graph_revision ==
             request.expected_current_graph_revision &&
         intent.candidate_retained_bundle_id ==
             request.candidate_retained_bundle_id &&
         intent.prepared_activation == request.prepared_activation &&
         intent.expected_admission_generation ==
             request.expected_admission_generation &&
         intent.parent_authoring_claim == request.parent_authoring_claim;
}

[[nodiscard]] core::CellAdmissionClaimIntentDTO
DirectAdmissionIntent(const PersistedManagerApplyIntent &durable) {
  const auto &intent = durable.intent;
  Require(intent.expected_admission_generation.has_value(),
          "direct Manager Apply lacks its admission generation");
  core::CellAdmissionClaimIntentDTO claim{
      core::CellAdmissionClaimKindDTO::kManagerApply,
      core::CellAdmissionOwnerRoleDTO::kManager,
      intent.request_context.request_owner_epoch,
      intent.apply_id.value(),
      intent.request_context.request_id,
      durable.intent_digest,
      *intent.expected_admission_generation,
      {},
      std::nullopt,
      std::nullopt,
  };
  Require(core::ValidateCellAdmissionClaimIntent(claim).ok(),
          "durable Manager Apply formed an invalid admission claim");
  return claim;
}

[[nodiscard]] core::CellAdmissionClaimDTO RequireHeldAdmission(
    const PersistedManagerApplyIntent &durable,
    core::IKernelCellAdmissionPort &cell_admission) {
  if (durable.intent.parent_authoring_claim.has_value()) {
    const auto &parent = *durable.intent.parent_authoring_claim;
    const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(
        {parent.intent, parent, std::nullopt});
    if (!receipt.ok()) {
      FailCause(receipt.error(),
                "Kernel parent-claim lookup failed: " + receipt.error().message,
                kKernelProducer);
    }
    Require(receipt.value().status ==
                    core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
                receipt.value().matched_claim == parent,
            "Program Apply parent authoring claim is not held exactly");
    return parent;
  }

  const auto intent = DirectAdmissionIntent(durable);
  const core::CellAdmissionRecoveryReceiptQueryDTO query{
      intent, std::nullopt, std::nullopt};
  const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(query);
  if (!receipt.ok()) {
    FailCause(receipt.error(),
              "Kernel Manager-claim lookup failed: " + receipt.error().message,
              kKernelProducer);
  }
  if (receipt.value().status ==
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
      receipt.value().matched_claim.has_value()) {
    return *receipt.value().matched_claim;
  }
  Require(receipt.value().status ==
              core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
          "Manager Apply admission claim is conflicting or already settled");
  const auto acquired = cell_admission.AcquireCellAdmissionClaim(intent);
  if (!acquired.ok()) {
    const auto &error = acquired.error();
    const auto reason = error.details.find("reason");
    FailCause(error,
              "Kernel rejected Manager Apply admission: " + error.code + ": " +
                  error.message +
                  (reason == error.details.end() ? std::string{}
                                                 : " (" + reason->second + ")"),
              kKernelProducer);
  }
  return acquired.value();
}

bool ReleaseAdmission(
    const PersistedManagerApplyIntent &durable,
    const PersistedManagerApplyTerminalOutcome &terminal,
    core::IKernelCellAdmissionPort &cell_admission,
    const std::optional<core::CellAdmissionClaimDTO> &original_parent_claim = {},
    const ManagerNamedApplyCoordinator::SuccessfulSettlementPreparation &prepare = {},
    const ManagerApplySettlementAttempt attempt = ManagerApplySettlementAttempt::kInitial) {
  const auto intent = durable.intent.parent_authoring_claim.has_value()
                          ? durable.intent.parent_authoring_claim->intent
                          : DirectAdmissionIntent(durable);
  const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(
      {intent, original_parent_claim, std::nullopt});
  if (!receipt.ok()) {
    FailCause(receipt.error(),
              "Kernel Manager-claim settlement lookup failed: " +
                  receipt.error().message,
              kKernelProducer);
  }
  if (receipt.value().status ==
      core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact) {
    return true;
  }
  Require(receipt.value().status ==
                  core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
              receipt.value().matched_claim.has_value(),
          "terminal Manager Apply has no exact held admission claim");
  const auto &claim = *receipt.value().matched_claim;
  if (terminal.terminal_outcome.succeeded() && prepare &&
      !prepare(durable.intent, attempt))
    return false;
  const auto settlement_kind = durable.intent.parent_authoring_claim.has_value()
                                   ? core::CellAdmissionSettlementKindDTO::kOwnerCommit
                                   : core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled;
  const core::CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {settlement_kind, terminal.terminal_outcome_digest},
  };
  const auto released = cell_admission.ReleaseCellAdmissionClaim(release);
  if (!released.ok()) {
    const auto reason = released.error().details.find("reason");
    FailCause(released.error(),
              "Kernel rejected Manager Apply settlement: " +
                  released.error().message +
                  (reason == released.error().details.end()
                       ? std::string{}
                       : " [" + reason->second + "]"),
              kKernelProducer);
  }
  return true;
}

} // namespace

core::Result<void> ValidateManagerPneumaticNoneProcessOwner(
    const deployment::RetainedBundle &predecessor, const deployment::SiteFacts &site_facts,
    const ManagerComponentProcessInventoryRequestDTO &request,
    const ManagerComponentProcessOccurrenceDTO &row) {
  try {
    Require(deployment::verify_retained_bundle_identity(predecessor) && request.valid() && row.valid() &&
                request.predecessor_projection_revision.retained_bundle_id == predecessor.retained_bundle_id() &&
                request.predecessor_projection_revision.graph_revision == predecessor.value().cell_graph.graph_revision &&
                row.process_owner_kind == ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal &&
                row.pneumatic_none_owner && row.installed_runtime_identity &&
                row.installed_runtime_identity == deployment::derive_component_runtime_identity(
          predecessor, site_facts, row.controllable_component_id),
            "actual_installed_state_unknown: pneumatic None occurrence differs from the immutable predecessor");
    const auto &graph = predecessor.value().cell_graph;
    const auto component = std::find_if(graph.controllable_components.begin(), graph.controllable_components.end(),
        [&](const auto &entry) { return entry.controllable_component_id == row.controllable_component_id; });
    Require(component != graph.controllable_components.end() &&
                std::find(component->runtime_unit_ids.begin(), component->runtime_unit_ids.end(),
                          request.runtime_unit_id) != component->runtime_unit_ids.end(),
            "actual_installed_state_unknown: pneumatic None component belongs to another runtime owner");
    const auto &proof = *row.pneumatic_none_owner;
    const auto binding = std::find_if(graph.control_bindings.begin(), graph.control_bindings.end(),
        [&](const auto &entry) { return entry.control_binding_id == proof.control_binding_id; });
    Require(binding != graph.control_bindings.end() &&
                binding->controllable_component_id == row.controllable_component_id &&
                binding->command_target_id == proof.command_target_id &&
                binding->control_binding_revision == proof.control_binding_revision &&
                binding->provider_runtime_unit_id == request.runtime_unit_id &&
                binding->capability_id == core::CapabilityId("tool.gripper.command"),
            "actual_installed_state_unknown: pneumatic None target or binding differs from its installed owner");
    // Decode the immutable selected ROS configuration through its existing
    // strict reader. One local gripper cannot exempt any other physical target
    // hosted by the same component from the unchanged process requirements.
    const auto selected = deployment::ros2_control_execution_runtime_contract::require_component_configuration(
        predecessor, request.runtime_unit_id, row.controllable_component_id);
    Require(selected.motion_target_limits.empty() && !selected.gripper_target_configurations.empty() &&
                std::all_of(selected.gripper_target_configurations.begin(), selected.gripper_target_configurations.end(),
                    [&](const auto &target) {
                      // D4: a lane-free tool (no controller profile) drives its
                      // valve through referenced I/O; it owns no process either.
                      return target.controllable_component_id == row.controllable_component_id &&
                             target.actuation_kind == "pneumatic" &&
                             (target.output_mode == core::GripperOutputMode::kNone ||
                              (target.pneumatic_io && selected.controller_profiles.empty()));
                    }),
            "actual_installed_state_unknown: pneumatic None occurrence has a physical controller requirement");
    for (const auto &entry : graph.control_bindings) {
      if (entry.controllable_component_id != row.controllable_component_id) continue;
      const bool owner_alias = entry.capability_id == core::CapabilityId("tool.gripper.command") ||
                               entry.capability_id == core::CapabilityId("tool.gripper.feedback");
      // Feedback is the same goal-fenced command-progress owner, never a
      // physical observation. Its own binding/capability identities remain
      // exact verified graph facts sealed by installed_runtime_identity above.
      // Only those distinct identities may differ from the command route.
      Require(owner_alias && entry.provider_runtime_unit_id == request.runtime_unit_id &&
                  entry.command_target_id == binding->command_target_id &&
                  entry.control_domain_id == binding->control_domain_id &&
                  entry.resource_bindings == binding->resource_bindings &&
                  entry.control_interface_contract_id == binding->control_interface_contract_id &&
                  entry.control_interface_revision == binding->control_interface_revision &&
                  entry.controller_topology_id == binding->controller_topology_id &&
                  entry.topology_revision == binding->topology_revision &&
                  entry.legal_control_route_ids == binding->legal_control_route_ids &&
                  entry.route_endpoint_projections == binding->route_endpoint_projections &&
                  entry.provider_instance_id == binding->provider_instance_id &&
                  entry.provider_manifest_id == binding->provider_manifest_id &&
                  entry.provider_manifest_revision == binding->provider_manifest_revision &&
                  entry.state_authority_id == binding->state_authority_id &&
                  entry.state_authority_class == binding->state_authority_class &&
                  entry.stop_evidence_policy_id == binding->stop_evidence_policy_id &&
                  entry.stop_evidence_policy_revision == binding->stop_evidence_policy_revision &&
                  entry.containment_granularity == binding->containment_granularity &&
                  entry.runtime_scale_route_qualifications == binding->runtime_scale_route_qualifications &&
                  std::count_if(selected.gripper_target_configurations.begin(), selected.gripper_target_configurations.end(),
                      [&](const auto &target) { return target.command_target_id == entry.command_target_id; }) == 1,
              "actual_installed_state_unknown: pneumatic None component has an unqualified target or provider binding");
    }
    return core::Result<void>::Ok();
  } catch (const std::exception &error) {
    return core::Result<void>::Err({"actual_installed_state_unknown", error.what(),
                                  {{"reason", "pneumatic_none_owner_mismatch"}}});
  }
}

bool ManagerNamedApplyRequestDTO::valid() const noexcept {
  const bool direct = expected_admission_generation.has_value() &&
                      expected_admission_generation->nonzero() &&
                      !parent_authoring_claim.has_value();
  const bool parent = !expected_admission_generation.has_value() &&
                      parent_authoring_claim.has_value() &&
                      core::ValidateCellAdmissionClaimIntent(
                          parent_authoring_claim->intent)
                          .ok() &&
                      parent_authoring_claim->intent.claim_kind ==
                          core::CellAdmissionClaimKindDTO::kAuthoringMutation &&
                      parent_authoring_claim->intent.owner_role ==
                          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner;
  return (scene_initialization == ManagerSceneInitialization::kPreserve ||
          scene_initialization == ManagerSceneInitialization::kInitialize) &&
         (direct || parent) && !request_context.request_id.empty() &&
         !request_context.request_owner_epoch.empty() &&
         !request_context.source_label.empty() &&
         (!request_context.correlation_id.has_value() ||
          !request_context.correlation_id->empty()) &&
         !deployment_artifact_id.empty() &&
         core::IsExactLowercaseSha256Digest(deployment_revision.value()) &&
         !expected_current_retained_bundle_id.empty() &&
         !expected_current_graph_revision.empty() &&
         !candidate_retained_bundle_id.empty() && prepared_activation.valid();
}

ManagerNamedApplyCoordinator::ManagerNamedApplyCoordinator(
    SqliteManagerApplyJournal &apply_journal,
    IManagerActiveRuntimePlanSource &active_plan_source,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    core::IActiveBundlePublicationStore &publication_store,
    core::IManagerSceneProjectionMaintenancePort &scene_owner,
    core::IManagerSceneProjectionBootstrapPort *bootstrap_scene_owner,
    core::IManagerKernelApplyFencePort &kernel_fence,
    core::IKernelCellAdmissionPort &cell_admission,
    IManagerSelectedRuntimeCompositionPort &runtime_composition,
    IRuntimeUnitProcessPort &process_port,
    IRuntimeUnitProgressPort &progress_port,
    ManagerPlannedMaintenanceCoordinator &planned_maintenance,
    core::RuntimeEpoch manager_runtime_epoch, std::string operating_system,
    std::vector<std::string> launch_environment_contracts,
    RuntimeObservationReducer::Clock clock_ns, ApplyIdFactory apply_id_factory,
    InitialSceneSource initial_scene_source,
    RuntimePreStartGuard pre_start_guard,
    ApplyProgressObserver apply_progress_observer,
    SuccessfulSettlementPreparation successful_settlement_preparation,
    deployment::SiteFacts site_facts)
    : apply_journal_(apply_journal), active_plan_source_(active_plan_source),
      retained_bundle_source_(retained_bundle_source),
      artifact_source_(artifact_source), publication_reader_(publication_store),
      scene_owner_(scene_owner), kernel_fence_(kernel_fence),
      cell_admission_(cell_admission),
      runtime_composition_port_(runtime_composition),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)),
      apply_id_factory_(std::move(apply_id_factory)),
      initial_scene_source_(std::move(initial_scene_source)),
      apply_progress_observer_(std::move(apply_progress_observer)),
      successful_settlement_preparation_(std::move(successful_settlement_preparation)),
      site_facts_(std::move(site_facts)),
      fence_coordinator_(apply_journal_, kernel_fence_, manager_runtime_epoch_, site_facts_),
      planned_maintenance_(planned_maintenance),
      active_runtime_coordinator_(active_plan_source, process_port,
                                  progress_port, clock_ns, pre_start_guard),
      runtime_coordinator_(apply_journal_, publication_store, artifact_source,
                           process_port, progress_port,
                           std::move(operating_system),
                           std::move(launch_environment_contracts),
                           std::move(clock_ns), std::move(pre_start_guard)),
      runtime_composition_coordinator_(apply_journal_, runtime_composition),
      selected_bundle_coordinator_(apply_journal_, publication_store,
                                   scene_owner),
      fence_release_coordinator_(apply_journal_, kernel_fence_, scene_owner,
                                 manager_runtime_epoch_) {
  static_cast<void>(bootstrap_scene_owner);
  Require(!manager_runtime_epoch_.empty(),
          "Manager runtime epoch cannot be empty");
  Require(static_cast<bool>(apply_id_factory_), "Apply ID factory is required");
  Require(static_cast<bool>(initial_scene_source_),
          "Apply initial-Scene source is required");
}

ManagerNamedApplyResult ManagerNamedApplyCoordinator::PendingSettlementResult(
    const ManagerApplyIntentDTO &intent, const bool replayed) const {
  return {intent.apply_id, intent.apply_generation,
          ManagerNamedApplyResultState::kPending, replayed, std::nullopt, true};
}

bool ManagerNamedApplyCoordinator::SettleSuccessfulAdmission(
    const PersistedManagerApplyIntent &durable,
    const PersistedManagerApplyTerminalOutcome &terminal,
    const ManagerApplySettlementAttempt attempt) {
  const auto &intent = durable.intent;
  const bool settled = ReleaseAdmission(durable, terminal, cell_admission_,
      std::nullopt, successful_settlement_preparation_, attempt);
  if (!settled) {
    pending_settlement_ = core::NamedApplyPublicationTransactionDTO{
        intent.apply_id, intent.apply_generation};
    return false;
  }
  pending_settlement_.reset();
  return true;
}

std::optional<core::NamedApplyPublicationTransactionDTO>
ManagerNamedApplyCoordinator::PendingSettlementForHeartbeat() {
  std::unique_lock lock(operation_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return std::nullopt;
  return pending_settlement_;
}

std::optional<ManagerNamedApplyResult>
ManagerNamedApplyCoordinator::RetryPendingSettlementOnHeartbeat(
    const core::NamedApplyPublicationTransactionDTO &expected) {
  // The independent heartbeat may not wait behind active Apply/Scene effects.
  // Keep the same pending reference for its next ordinary cadence instead.
  std::unique_lock lock(operation_mutex_, std::try_to_lock);
  if (!lock.owns_lock()) return std::nullopt;
  if (!pending_settlement_ || *pending_settlement_ != expected) return std::nullopt;
  const auto reference = *pending_settlement_;
  try {
    const auto durable = apply_journal_.ReadIntent(reference.apply_id, reference.apply_generation);
    const auto terminal = apply_journal_.ReadTerminalOutcome(reference.apply_id, reference.apply_generation);
    Require(durable && terminal && terminal->terminal_outcome.succeeded(),
            "pending Apply settlement lost its exact successful journal record");
    if (!SettleSuccessfulAdmission(*durable, *terminal,
                                  ManagerApplySettlementAttempt::kOrdinaryHeartbeat))
      return PendingSettlementResult(durable->intent, true);
    if (captured_apply_inputs_ && captured_apply_inputs_->apply_id == reference.apply_id &&
        captured_apply_inputs_->apply_generation == reference.apply_generation)
      captured_apply_inputs_.reset();
    return ManagerNamedApplyResult{reference.apply_id, reference.apply_generation,
        ManagerNamedApplyResultState::kSucceeded, true, terminal->terminal_outcome};
  } catch (...) {
    // Source/read/validation errors and uncertain release are not permission
    // to retry. Leave actual claim/terminal evidence for the old diagnosis path;
    // only a known preparation-false result retains this scheduling reference.
    pending_settlement_.reset();
    throw;
  }
}

core::ApplyId ManagerNamedApplyCoordinator::MintApplyId() {
  for (std::size_t attempt = 0U; attempt < kMaximumApplyIdMintAttempts;
       ++attempt) {
    auto apply_id = apply_id_factory_();
    if (!apply_id.empty() &&
        !apply_journal_.ReadApplyGenerationHighWater(apply_id).has_value()) {
      return apply_id;
    }
  }
  Fail("Apply ID factory did not produce a fresh bounded identity");
}

bool ManagerNamedApplyCoordinator::HasOriginalConfigurationCapture(
    const ManagerApplyIntentDTO &intent) const {
  if (!captured_apply_inputs_ ||
      !captured_apply_inputs_->original_parent_claim ||
      !captured_apply_inputs_->original_predecessor_publication ||
      captured_apply_inputs_->apply_id != intent.apply_id ||
      captured_apply_inputs_->apply_generation != intent.apply_generation ||
      !captured_apply_inputs_->predecessor || !captured_apply_inputs_->candidate)
    return false;
  const auto stored = apply_journal_.ReadIntent(intent.apply_id, intent.apply_generation);
  const auto &captured = *captured_apply_inputs_;
  Require(stored && stored->intent == intent &&
              captured.predecessor->retained_bundle_id() == intent.expected_active_projection_revision.retained_bundle_id &&
              captured.candidate->retained_bundle_id() == intent.candidate_retained_bundle_id &&
              captured.original_predecessor_publication->valid() &&
              core::SameActiveDeployment(captured.original_predecessor_publication->request.candidate_projection_revision,
                                         intent.expected_active_projection_revision),
          "configuration capture differs from its original immutable request/head");
  if (intent.parent_authoring_claim) {
    Require(captured.original_parent_claim == intent.parent_authoring_claim,
            "configuration capture differs from its original authoring parent");
  } else {
    Require(captured.original_parent_claim->intent == DirectAdmissionIntent(*stored),
            "configuration capture differs from its original direct Manager claim");
  }
  return true;
}

core::QualifiedSceneConfigurationAdoptionRequestDTO
ManagerNamedApplyCoordinator::CaptureOriginalConfigurationAdoption(
    const ManagerApplyIntentDTO &intent,
    const std::optional<core::ApplyFenceAcquireAttemptQueryDTO> &staging_attempt) {
  Require(HasOriginalConfigurationCapture(intent),
          "configuration Scene adoption requires the original process capture");
  const auto &captured = *captured_apply_inputs_;
  const auto status = apply_journal_.ReadStatus(intent.apply_id, intent.apply_generation);
  Require(status && status->fence_attempt && status->fence_acceptance &&
              status->latest_runtime_staged_ready &&
              status->intent.intent == intent &&
              status->fence_acceptance->fence_attempt_digest == status->fence_attempt->fence_attempt_digest,
          "configuration Scene adoption lacks exact stored parent/fence/host evidence");
  const auto &claim = *captured.original_parent_claim;
  const core::CellAdmissionRecoveryReceiptQueryDTO claim_query{claim.intent, claim, std::nullopt};
  const auto claim_receipt = cell_admission_.GetCellAdmissionRecoveryReceipt(claim_query);
  Require(core::ValidateCellAdmissionRecoveryReceiptQuery(claim_query).ok() && claim_receipt.ok() &&
              core::ValidateCellAdmissionRecoveryReceipt(claim_receipt.value()).ok() &&
              claim_receipt.value().status == core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
              claim_receipt.value().matched_claim == claim &&
              !claim_receipt.value().matched_release && !claim_receipt.value().matched_reconcile_settlement,
          "configuration original claim is no longer held exactly");
  const auto &fence = status->fence_acceptance->accepted_fence;
  const core::ConfigurationFenceStatusQueryDTO query{
      {intent.apply_id, intent.apply_generation}, fence.configuration_fence_id,
      fence.configuration_fence_generation, fence.captured_scope, manager_runtime_epoch_};
  Require(query.valid(), "configuration parent fence query is invalid");
  if (staging_attempt) {
    Require(staging_attempt->valid() &&
                staging_attempt->persisted_intent == status->fence_attempt->fence_intent &&
                staging_attempt->persisted_attempt_digest == status->fence_attempt->fence_attempt_digest &&
                staging_attempt->manager_runtime_epoch == manager_runtime_epoch_ &&
                staging_attempt->persisted_intent.owner_request_context == intent.request_context &&
                staging_attempt->persisted_intent.owner_transaction == query.owner_transaction &&
                staging_attempt->persisted_intent.scope == query.expected_scope,
            "configuration staging query differs from its original stored attempt");
    // Observe the real ledger directly, without Acquire retry/persistence or
    // candidate-installed release reconciliation.
    const auto held = kernel_fence_.ReconcileAcquireAttempt(*staging_attempt);
    Require(held.matches(*staging_attempt) &&
                held.state == core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact &&
                held.admission_fenced && held.accepted_fence == fence &&
                held.execution_host_epoch.publication_generation ==
                    captured.original_predecessor_publication->publication_generation &&
                std::any_of(status->latest_runtime_staged_ready->staged_ready.runtime_units.begin(),
                            status->latest_runtime_staged_ready->staged_ready.runtime_units.end(),
                            [&](const auto &runtime) {
                              return runtime.valid() && runtime.runtime_unit_id == held.execution_host_epoch.runtime_unit_id &&
                                  runtime.runtime_epoch == held.execution_host_epoch.runtime_epoch;
                            }),
            "configuration staging fence is not actually held by the staged host");
  } else {
    const auto held = kernel_fence_.QueryStatus(query);
    Require(held.matches(query) && held.state == core::ConfigurationFenceState::kHeld && held.admission_fenced,
            "configuration Scene adoption fence is no longer held exactly");
  }
  return {{{intent.apply_id, intent.apply_generation}, status->intent.intent_digest, claim, query},
      *captured.original_predecessor_publication, intent.expected_active_projection_revision,
      {intent.expected_active_projection_revision.scene_revision, intent.cell_id}, captured.candidate->value(),
      detail::DeriveCandidateNativeProjectionBindings(captured.candidate->value(),
          status->latest_runtime_staged_ready->staged_ready)};
}

ManagerNamedApplyResult ManagerNamedApplyCoordinator::ContinueOriginalConfiguration(
    const PersistedManagerApplyIntent &durable, const bool request_replayed) {
  const auto &intent = durable.intent;
  Require(HasOriginalConfigurationCapture(intent), "configuration lost its original capture");
  auto &captured = *captured_apply_inputs_;
  const auto pending = [&] { return ManagerNamedApplyResult{intent.apply_id, intent.apply_generation,
      ManagerNamedApplyResultState::kPending, request_replayed, std::nullopt}; };
  try {
    const auto status = apply_journal_.ReadStatus(intent.apply_id, intent.apply_generation);
    Require(status && status->fence_attempt && status->fence_acceptance,
            "configuration staging lacks original fence acceptance");
    const core::ApplyFenceAcquireAttemptQueryDTO attempt{
        status->fence_attempt->fence_intent, status->fence_attempt->fence_attempt_digest, manager_runtime_epoch_};
    const core::ProjectionRevisionDTO target{intent.candidate_retained_bundle_id,
        intent.candidate_graph_revision, intent.expected_active_projection_revision.scene_revision};
    if (!captured.original_preparation_completed) {
      const auto request = CaptureOriginalConfigurationAdoption(intent, attempt);
      const auto before = publication_reader_.ReadActiveBundlePublication(
          request.expected_predecessor_publication.publication_generation);
      Require(before.ok() && before.value() == request.expected_predecessor_publication,
              "configuration staging predecessor is not the current head");
      std::vector<core::ProjectionHealthDTO> health;
      if (!captured.original_scene_stage_consumed) {
        captured.original_configuration_adoption = request;
        captured.original_scene_stage_consumed = true;
        health = scene_owner_.StageOriginalManagerConfigurationAdoption(request, attempt);
        captured.original_scene_stage_completed = true;
      } else {
        Require(captured.original_scene_stage_completed && captured.original_configuration_adoption == request,
                "configuration staging has an uncertain outcome or changed identity");
        health = scene_owner_.ReadOriginalManagerConfigurationAdoptionProjectionHealth(request, attempt);
      }
      Require(CaptureOriginalConfigurationAdoption(intent, attempt) == request &&
                  health.size() == request.execution_host_bindings.size(),
              "configuration staging changed identity or returned incomplete participants");
      const auto after = publication_reader_.ReadActiveBundlePublication(
          request.expected_predecessor_publication.publication_generation);
      Require(after.ok() && after.value() == request.expected_predecessor_publication,
              "configuration staging changed the public head");
      bool ready = true;
      for (std::size_t index = 0; index < health.size(); ++index) {
        const auto &row = health[index];
        Require(row.valid() && row.execution_host_binding == request.execution_host_bindings[index] && !row.error,
                "configuration staging returned foreign or failed native evidence");
        if (!row.healthy_for_identity(target)) {
          Require(row.state == core::ProjectionHealthState::kRebuilding ||
                      row.state == core::ProjectionHealthState::kUninitialized,
                  "configuration staging returned unavailable native evidence");
          ready = false;
        }
      }
      if (!ready) return pending();
      const auto applied = runtime_composition_coordinator_.ApplyOnHost(
          intent.apply_id, intent.apply_generation, *captured.predecessor, *captured.candidate);
      if (captured.original_prepare_acknowledgement && !captured.prepare_acknowledgement_consumed) {
        captured.prepare_acknowledgement_consumed = true;
        captured.original_prepare_acknowledgement(intent, *captured.candidate, applied);
      }
      captured.original_preparation_completed = true;
    }
    if (!captured.original_scene_begin_consumed) {
      const auto request = CaptureOriginalConfigurationAdoption(intent);
      Require(captured.original_scene_stage_completed && captured.original_configuration_adoption == request,
              "configuration Begin differs from its original staged identity");
      captured.original_scene_begin_consumed = true;
      const auto begun = scene_owner_.BeginOriginalManagerConfigurationAdoption(request);
      Require(begun.valid() && begun.apply_id == intent.apply_id && begun.apply_generation == intent.apply_generation &&
                  begun.state == core::ProjectionMaintenanceInitializationState::kInitialized &&
                  begun.ready_for_publication() && !begun.publication_receipt && !begun.effects_unknown && !begun.first_error &&
                  core::SameProjectionIdentity(begun.predecessor_projection_revision,
                                               intent.expected_active_projection_revision) &&
                  core::SameProjectionIdentity(begun.target_projection_revision, target) &&
                  SameSnapshotScene(begun.authoritative_snapshot_identity, request.expected_snapshot_identity) &&
                  begun.execution_host_bindings == request.execution_host_bindings &&
                  CaptureOriginalConfigurationAdoption(intent) == request,
              "configuration Begin returned foreign or incomplete preservation evidence");
    }
    static_cast<void>(selected_bundle_coordinator_.PublishOriginalConfiguration(
        intent.apply_id, intent.apply_generation, *captured.predecessor, *captured.candidate));
    return FinishOriginalConfigurationPublication(durable, request_replayed);
  } catch (const std::exception &error) {
    if (apply_journal_.ReadTerminalOutcome(intent.apply_id, intent.apply_generation)) throw;
    // The host's typed refusal of the candidate (nothing installed or
    // published, nothing changed) is a known before-effect failure: like a
    // pre-fence failure (CLAIM-1) it releases its fence and claim at once,
    // so authoring and New work again. Any other failure keeps them.
    const auto disposition = PreparationFailureDisposition(error);
    const bool host_refused = disposition == ManagerApplyEffectDisposition::kKnown &&
                              !captured.original_preparation_completed;
    auto result = RetainTerminalFailure(apply_journal_, durable, "configuration_preservation_failed",
        error, disposition, request_replayed,
        host_refused ? ManagerApplyEffectBoundary::kBeforeEffect
                     : ManagerApplyEffectBoundary::kUnknown);
    if (host_refused) {
      try {
        ReleaseFailedApply(intent.apply_id, intent.apply_generation,
            core::RequestId("host-refusal-release." + intent.apply_id.value()),
            "manager: execution host refused the candidate before any effect", false);
      } catch (const std::exception &release_error) {
        // The failure stays retained; its fence and claim stay held.
        std::cerr << "Manager Apply " << intent.apply_id.value()
                  << ": host refusal could not release its fence and claim: "
                  << release_error.what() << '\n';
      }
    }
    return result;
  }
}

ManagerNamedApplyResult ManagerNamedApplyCoordinator::FinishOriginalConfigurationPublication(
    const PersistedManagerApplyIntent &durable, const bool request_replayed) {
  const auto &intent = durable.intent;
  Require(HasOriginalConfigurationCapture(intent), "published configuration lost its original capture");
  try {
    auto &captured = *captured_apply_inputs_;
    Require(captured.original_scene_begin_consumed && captured.original_configuration_adoption,
            "configuration publication has no original preserved Scene operation");
    const auto publication = apply_journal_.ReadPublicationReceipt(intent.apply_id, intent.apply_generation);
    Require(publication.has_value(), "configuration publication has no durable receipt");
    const auto &receipt = publication->publication.publication_receipt;
    const core::ProjectionRevisionDTO target{intent.candidate_retained_bundle_id,
        intent.candidate_graph_revision, intent.expected_active_projection_revision.scene_revision};
    Require(receipt.matches_named_apply(intent.apply_id, intent.apply_generation, target) &&
                receipt.request.expected_previous_publication_generation ==
                    captured.original_predecessor_publication->publication_generation,
            "configuration publication differs from the original candidate/head");
    const auto head = publication_reader_.ReadActiveBundlePublication(receipt.publication_generation);
    Require(head.ok() && head.value() == receipt, "configuration receipt is not the actual current head");
    if (!captured.original_scene_complete_consumed) {
      Require(CaptureOriginalConfigurationAdoption(intent) == *captured.original_configuration_adoption,
              "configuration identity changed before Scene Complete");
      captured.original_scene_complete_consumed = true;
      const auto completed = scene_owner_.CompleteOriginalManagerConfigurationAdoption(
          *captured.original_configuration_adoption, receipt);
      Require(completed.valid() && completed.apply_id == intent.apply_id && completed.apply_generation == intent.apply_generation &&
                  completed.state == core::ProjectionMaintenanceInitializationState::kCompleted &&
                  core::SamePublicationIdentity(completed.publication_receipt, receipt) &&
                  completed.initial_scene_installed &&
                  !completed.effects_unknown && !completed.first_error &&
                  core::SameProjectionIdentity(completed.predecessor_projection_revision,
                                               intent.expected_active_projection_revision) &&
                  core::SameProjectionIdentity(completed.target_projection_revision, target) &&
                  SameSnapshotScene(completed.authoritative_snapshot_identity,
                                    captured.original_configuration_adoption->expected_snapshot_identity) &&
                  completed.execution_host_bindings == captured.original_configuration_adoption->execution_host_bindings &&
                  CaptureOriginalConfigurationAdoption(intent) == *captured.original_configuration_adoption,
              "configuration Scene Complete returned foreign or incomplete preservation evidence");
      const auto after = publication_reader_.ReadActiveBundlePublication(receipt.publication_generation);
      Require(after.ok() && after.value() == receipt, "configuration head changed across Scene Complete");
      captured.original_scene_completed = completed;
    }
    Require(captured.original_scene_completed.has_value(), "configuration Scene completion outcome is uncertain");
    const auto released = fence_release_coordinator_.ReleaseOrRecover(intent.apply_id, intent.apply_generation);
    const auto terminal = apply_journal_.ReadTerminalOutcome(intent.apply_id, intent.apply_generation);
    Require(terminal.has_value(), "configuration release lacks its actual terminal evidence");
    if (!SettleSuccessfulAdmission(durable, *terminal, ManagerApplySettlementAttempt::kInitial))
      return PendingSettlementResult(intent, request_replayed);
    return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kSucceeded,
        request_replayed, released.durable_terminal_outcome.record.terminal_outcome};
  } catch (const std::exception &error) {
    if (apply_journal_.ReadTerminalOutcome(intent.apply_id, intent.apply_generation)) throw;
    return RetainTerminalFailure(apply_journal_, durable, "configuration_settlement_failed",
        error, ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }
}

ManagerNamedApplyResult ManagerNamedApplyCoordinator::AcceptOrReplay(
    const ManagerNamedApplyRequestDTO &request,
    OriginalPrepareAcknowledgement original_prepare_acknowledgement) {
  std::lock_guard lock(operation_mutex_);
  Require(request.valid(), "named Apply request is invalid");

  if (const auto existing = apply_journal_.ReadRequest(
          request.request_context.request_owner_epoch,
          request.request_context.request_id);
      existing.has_value()) {
    Require(MatchesRequest(existing->intent, request),
            "public request identity is already bound to another Apply");
    if (pending_settlement_ == core::NamedApplyPublicationTransactionDTO{
            existing->intent.apply_id, existing->intent.apply_generation})
      return PendingSettlementResult(existing->intent, true);
    const auto terminal = apply_journal_.ReadTerminalOutcome(
        existing->intent.apply_id, existing->intent.apply_generation);
    return {existing->intent.apply_id, existing->intent.apply_generation,
            terminal.has_value()
                ? ResultStateFromTerminal(terminal->terminal_outcome)
                : ManagerNamedApplyResultState::kPending,
            true,
            terminal.has_value() ? std::optional(terminal->terminal_outcome)
                                 : std::nullopt};
  }

  if (pending_settlement_)
    throw ManagerNamedApplyBusyError("another successful Apply retains its original admission claim");

  const auto candidate =
      LoadBundle(retained_bundle_source_, request.candidate_retained_bundle_id,
                 "candidate");
  const auto &candidate_value = candidate->value();
  Require(candidate_value.deployment_artifact_id ==
                  request.deployment_artifact_id &&
              candidate_value.deployment_revision ==
                  request.deployment_revision,
          "selected candidate does not match the requested deployment "
          "identity and revision");
  Require(candidate_value.cell_graph.cell_id == apply_journal_.cell_id(),
          "selected candidate belongs to another cell");
  const auto inventory = apply_journal_.ReadStatusInventory();
  for (const auto &status : inventory) {
    if (!IsTerminal(status)) {
      throw ManagerNamedApplyBusyError(
          "another named Apply remains nonterminal on this Manager lane");
    }
  }

  const auto active_plan = active_plan_source_.ReadCurrentPlan();
  Require(active_plan.valid(), "current active runtime plan is invalid");
  const auto &active_revision =
      active_plan.active_publication.request.candidate_projection_revision;
  Require(active_revision.retained_bundle_id ==
                  request.expected_current_retained_bundle_id &&
              active_revision.graph_revision ==
                  request.expected_current_graph_revision,
          "named Apply expected_current does not match the active pointer");

  const auto predecessor =
      LoadBundle(retained_bundle_source_, active_revision.retained_bundle_id,
                 "predecessor");
  Require(predecessor->value().cell_graph.cell_id == apply_journal_.cell_id() &&
              predecessor->value().cell_graph.graph_revision ==
                  active_revision.graph_revision,
          "selected predecessor does not match the active cell graph");

  const auto scene_revision = scene_owner_.CurrentProjectionRevision();
  Require(scene_revision.valid() &&
              SameDeploymentRevision(scene_revision, active_revision),
          "logical-scene predecessor does not match the active Deployment");

  ManagerApplyIntentDTO intent{
      request.request_context,
      MintApplyId(),
      core::ApplyGeneration(1U),
      apply_journal_.cell_id(),
      scene_revision,
      candidate->retained_bundle_id(),
      candidate_value.cell_graph.graph_revision,
      request.prepared_activation,
      request.expected_admission_generation,
      request.parent_authoring_claim,
      request.scene_initialization,
  };
  Require(intent.valid(), "Manager could not construct a valid Apply intent");
  const auto durable_intent = apply_journal_.EnsureIntent(intent).record;
  // Claim the exact idle admission before exposing this Apply as pending.
  // Publishing pending closes readiness; doing that first invalidates the
  // admission generation carried by the accepted request itself.
  std::optional<core::CellAdmissionClaimDTO> accepted_claim;
  try {
    accepted_claim = RequireHeldAdmission(durable_intent, cell_admission_);
  } catch (const std::exception &error) {
    // No runtime lifecycle action was issued on this path. Whether an
    // admission claim exists is a separate fact: only a typed known refusal
    // proves no admission effect (AdmissionRefusalBoundary). Any other failure
    // may have left a claim, so its effects are unknown: the Apply is retained
    // as unknown_effect, admission stays closed and nothing releases it.
    const auto boundary = AdmissionRefusalBoundary(error);
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "apply_admission_failed", error,
        boundary == ManagerApplyEffectBoundary::kBeforeEffect
            ? ManagerApplyEffectDisposition::kKnown
            : ManagerApplyEffectDisposition::kUnknown,
        false, boundary);
  }
  // Only this fresh call owns these verified inputs; a restarted Manager
  // cannot recreate them.
  captured_apply_inputs_.emplace(CapturedApplyInputs{
      intent.apply_id, intent.apply_generation, predecessor, candidate,
      std::move(original_prepare_acknowledgement)});
  captured_apply_inputs_->original_parent_claim = accepted_claim;
  captured_apply_inputs_->original_predecessor_publication = active_plan.active_publication;
  if (apply_progress_observer_) {
    apply_progress_observer_(durable_intent.intent);
  }
  return {intent.apply_id, intent.apply_generation,
          ManagerNamedApplyResultState::kPending, false, std::nullopt};
}

ManagerNamedApplyResult
ManagerNamedApplyCoordinator::Continue(const ManagerApplyIntentDTO &intent,
                                       const bool request_replayed) {
  Require(intent.valid() && intent.cell_id == apply_journal_.cell_id(),
          "durable Apply intent is invalid or belongs to another cell");
  const bool original_configuration = HasOriginalConfigurationCapture(intent);
  const auto durable_intent = apply_journal_.EnsureIntent(intent).record;
  if (apply_progress_observer_)
    apply_progress_observer_(durable_intent.intent);

  if (const auto terminal = apply_journal_.ReadTerminalOutcome(
          intent.apply_id, intent.apply_generation);
      terminal.has_value()) {
    if (pending_settlement_ == core::NamedApplyPublicationTransactionDTO{
            intent.apply_id, intent.apply_generation})
      return PendingSettlementResult(intent, request_replayed);
    return {intent.apply_id, intent.apply_generation,
            ResultStateFromTerminal(terminal->terminal_outcome),
            request_replayed, terminal->terminal_outcome};
  }

  if (original_configuration &&
      apply_journal_.ReadPublicationReceipt(intent.apply_id, intent.apply_generation))
    return FinishOriginalConfigurationPublication(durable_intent, request_replayed);

  const auto retain_pre_effect_failure =
      [&](const std::string &code, const core::TypedCause &cause,
          const ManagerApplyEffectDisposition disposition,
          const ManagerApplyEffectBoundary boundary) {
        auto result = RetainTerminalFailure(apply_journal_, durable_intent,
                                            code, cause, disposition,
                                            request_replayed, boundary);
        if (disposition == ManagerApplyEffectDisposition::kKnown) {
          const auto terminal = apply_journal_.ReadTerminalOutcome(
              intent.apply_id, intent.apply_generation);
          Require(terminal.has_value(),
                  "pre-effect failure lacks terminal evidence");
          ReleaseAdmission(durable_intent, *terminal, cell_admission_);
        }
        return result;
      };

  // Reading the current state for the fence changes nothing: before a fence
  // attempt is journaled its failure is known pre-effect and releases the
  // admission claim (core rule 3); after one, its effect stays unknown.
  const auto retain_evidence_failure = [&](const std::exception &error) {
    if (apply_journal_.ReadFenceAttempt(intent.apply_id, intent.apply_generation))
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "apply_fence_evidence_failed", error,
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    return retain_pre_effect_failure(
        "apply_fence_evidence_failed",
        core::CauseOf(error, "apply_fence_evidence_failed", kApplyProducer),
        ManagerApplyEffectDisposition::kKnown,
        ManagerApplyEffectBoundary::kBeforeEffect);
  };

  Require(captured_apply_inputs_.has_value() &&
              captured_apply_inputs_->apply_id == intent.apply_id &&
              captured_apply_inputs_->apply_generation ==
                  intent.apply_generation &&
              captured_apply_inputs_->predecessor != nullptr &&
              captured_apply_inputs_->candidate != nullptr,
          "named Apply continuation lacks its process-local captured inputs");
  const auto &predecessor = *captured_apply_inputs_->predecessor;
  const auto &candidate = *captured_apply_inputs_->candidate;

  if (!apply_journal_
           .ReadFenceAttempt(intent.apply_id, intent.apply_generation)
           .has_value()) {
    const auto refusal = planned_maintenance_.Preflight(
        intent, intent.expected_active_projection_revision);
    if (refusal.has_value()) {
      Require(refusal->valid() && refusal->terminal_failure(),
              "planned maintenance preflight returned an invalid refusal");
      return retain_pre_effect_failure(
          refusal->failure_code,
          PlainCause(refusal->failure_code, refusal->failure_message),
          refusal->effect_disposition, MaintenanceBoundary(*refusal));
    }
  }

  try {
    static_cast<void>(RequireHeldAdmission(durable_intent, cell_admission_));
  } catch (const std::exception &error) {
    const auto disposition =
        apply_journal_
                .ReadFenceAttempt(intent.apply_id, intent.apply_generation)
                .has_value()
            ? ManagerApplyEffectDisposition::kUnknown
            : ManagerApplyEffectDisposition::kKnown;
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "apply_admission_failed", error,
        disposition, request_replayed);
  }

  std::optional<ManagerActiveRuntimePlanDTO> active_plan;
  try {
    active_plan.emplace(active_plan_source_.ReadCurrentPlan());
    Require(active_plan->valid() &&
                SameDeploymentRevision(
                    active_plan->active_publication.request
                        .candidate_projection_revision,
                    intent.expected_active_projection_revision),
            "predecessor publication changed before initialization");
  } catch (const std::exception &error) {
    return retain_evidence_failure(error);
  }

  const auto host_continuity = VerifyExecutionHostContinuity(
      predecessor, candidate, artifact_source_);
  if (!host_continuity.ok()) {
    return retain_pre_effect_failure(
        host_continuity.error().code,
        {host_continuity.error(), std::string(kApplyProducer)},
        ManagerApplyEffectDisposition::kKnown,
        ManagerApplyEffectBoundary::kBeforeEffect);
  }

  std::optional<ManagerApplyFenceExpectationDTO> expectation;
  try {
    expectation.emplace(FenceExpectation(
        intent, predecessor, candidate, *active_plan, apply_journal_,
        kernel_fence_, manager_runtime_epoch_,
        // Component processes and lifecycles live in the execution host.
        // Every Apply whose predecessor runs it inspects them; a predecessor
        // without it has none to inspect.
        std::any_of(active_plan->runtime_units.begin(),
                    active_plan->runtime_units.end(),
                    [](const auto &unit) {
                      return unit.runtime_profile_id.value() ==
                             deployment::
                                 ros2_control_execution_runtime_contract::
                                     kRuntimeProfileId;
                    })
            ? &runtime_composition_port_
            : nullptr,
        retained_bundle_source_, site_facts_));
  } catch (const ActiveComponentApplyRefusal &error) {
    return retain_pre_effect_failure(
        "apply_requires_inactive_component",
        PlainCause("apply_requires_inactive_component", error.what()),
        ManagerApplyEffectDisposition::kKnown,
        ManagerApplyEffectBoundary::kBeforeEffect);
  } catch (const std::exception &error) {
    return retain_evidence_failure(error);
  }

  if (!apply_journal_
           .ReadRuntimeConvergenceIntent(intent.apply_id,
                                         intent.apply_generation)
           .has_value()) {
    std::optional<ManagerApplyFenceAcquireResult> acquired_fence;
    try {
      acquired_fence.emplace(fence_coordinator_.AcquireOrRecoverFence(
          intent, predecessor, candidate, *expectation));
    } catch (const ManagerApplyFenceEffectUncertainError &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "apply_fence_effect_unknown",
          error, ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "apply_fence_acquire_failed",
          error, ManagerApplyEffectDisposition::kKnown,
          request_replayed);
    }
    if (acquired_fence->path !=
            ManagerApplyFenceAcquirePath::kAcquiredFromNewAttempt &&
        acquired_fence->path !=
            ManagerApplyFenceAcquirePath::kRetriedAfterAuthoritativeNotSeen) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "planned_maintenance_interrupted",
          "maintenance admission belongs to an earlier Manager incarnation; "
          "effects will not be resumed or repeated",
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }

    const auto maintenance = planned_maintenance_.EnterIdleMaintenance(
        intent, intent.expected_active_projection_revision);
    if (!maintenance.valid()) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent,
          "planned_maintenance_result_invalid",
          "planned maintenance returned invalid evidence",
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
    if (maintenance.terminal_failure()) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, maintenance.failure_code,
          maintenance.failure_message,
          maintenance.effect_disposition, request_replayed,
          MaintenanceBoundary(maintenance));
    }
  }

  try {
    const auto runtime = runtime_coordinator_.ConvergeOrRecover(
        intent, predecessor, candidate,
        active_plan->active_publication.publication_generation);
    if (runtime.state == ManagerRuntimeConvergenceResultState::kPending) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    }
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent,
        "planned_maintenance_initialization_failed", error,
        ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }

  Require(original_configuration,
          "configuration Apply lacks its original preserved-Scene capture");
  return ContinueOriginalConfiguration(durable_intent, request_replayed);
}

std::optional<ManagerNamedApplyResult>
ManagerNamedApplyCoordinator::RecoverIncompleteApply() {
  std::lock_guard lock(operation_mutex_);
  if (pending_settlement_)
    return ManagerNamedApplyResult{
        pending_settlement_->apply_id, pending_settlement_->apply_generation,
        ManagerNamedApplyResultState::kPending, true, std::nullopt, true};
  const auto inventory = apply_journal_.ReadStatusInventory();
  const PersistedManagerApplyStatus *pending = nullptr;
  for (const auto &status : inventory) {
    if (IsTerminal(status))
      continue;
    Require(pending == nullptr,
            "multiple nonterminal Applies contradict the sole progression owner");
    pending = &status;
  }
  if (pending == nullptr) {
    captured_apply_inputs_.reset();
    return std::nullopt;
  }

  if (captured_apply_inputs_.has_value() &&
      captured_apply_inputs_->apply_id == pending->intent.intent.apply_id &&
      captured_apply_inputs_->apply_generation ==
          pending->intent.intent.apply_generation) {
    auto result = Continue(pending->intent.intent, true);
    if (result.state != ManagerNamedApplyResultState::kPending) {
      captured_apply_inputs_.reset();
    }
    return result;
  }

  return RetainTerminalFailure(
      apply_journal_, pending->intent, "planned_maintenance_interrupted",
      "Manager reopened a nonterminal maintenance operation; effects remain "
      "unknown and admission stays closed until explicit diagnosis and a "
      "later authorized operation",
      ManagerApplyEffectDisposition::kUnknown, true);
}

const char *ToString(const FailedApplyAcknowledgementState state) noexcept {
  switch (state) {
  case FailedApplyAcknowledgementState::kAvailable: return "available";
  case FailedApplyAcknowledgementState::kAcknowledged: return "acknowledged";
  case FailedApplyAcknowledgementState::kUnavailable: return "unavailable";
  }
  return "unavailable";
}

FailedApplyAcknowledgementState ManagerNamedApplyCoordinator::FailedApplyAcknowledgementStatus(
    const core::ApplyId &apply_id, const core::ApplyGeneration apply_generation) {
  // This is advisory status, not a release decision. The journal serializes
  // its own reads; the acknowledgement rechecks under operation_mutex_.
  try {
    const auto status = apply_journal_.ReadStatus(apply_id, apply_generation);
    if (!status || !FailedApplyAcknowledgementEligible(*status))
      return FailedApplyAcknowledgementState::kUnavailable;
    const auto release = apply_journal_.ReadFailedApplyRelease(apply_id, apply_generation);
    if (!release) return FailedApplyAcknowledgementState::kAvailable;
    return release->acknowledged ? FailedApplyAcknowledgementState::kAcknowledged
                                 : FailedApplyAcknowledgementState::kUnavailable;
  } catch (...) {
    // An unreadable failure or affected scope is not permission.
    return FailedApplyAcknowledgementState::kUnavailable;
  }
}

void ManagerNamedApplyCoordinator::AcknowledgeFailedApply(
    const core::ApplyId &apply_id, const core::ApplyGeneration apply_generation,
    const core::RequestId &request_id, const std::string &source_label) {
  std::lock_guard lock(operation_mutex_);
  ReleaseFailedApply(apply_id, apply_generation, request_id, source_label, false);
}

std::optional<core::NamedApplyPublicationTransactionDTO>
ManagerNamedApplyCoordinator::ReleaseFailedApplyForNew(const core::RequestId &request_id,
                                                       const std::string &source_label) {
  std::lock_guard lock(operation_mutex_);
  for (const auto &status : apply_journal_.ReadStatusInventory()) {
    const auto &intent = status.intent.intent;
    if (!FailedApplyNewReleaseEligible(status) ||
        apply_journal_.ReadFailedApplyRelease(intent.apply_id, intent.apply_generation))
      continue;
    // The failed Apply that holds the cell is the one whose claim the Kernel
    // still holds; an earlier one's ended with its Kernel.
    const auto &parent = intent.parent_authoring_claim;
    const auto claim = cell_admission_.GetCellAdmissionRecoveryReceipt(
        {parent ? parent->intent : DirectAdmissionIntent(status.intent), parent, std::nullopt});
    if (!claim.ok()) FailCause(claim.error(), claim.error().message, kKernelProducer);
    if (claim.value().status != core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact) continue;
    ReleaseFailedApply(intent.apply_id, intent.apply_generation, request_id, source_label, true);
    return core::NamedApplyPublicationTransactionDTO{intent.apply_id, intent.apply_generation};
  }
  return std::nullopt;
}

void ManagerNamedApplyCoordinator::ReleaseFailedApply(
    const core::ApplyId &apply_id, const core::ApplyGeneration apply_generation,
    const core::RequestId &request_id, const std::string &source_label, const bool for_new) {
  std::optional<PersistedManagerApplyStatus> status;
  std::optional<core::ConfigurationFenceReleaseDTO> release_request;
  // This block makes only read-only checks. Only a refusal from here is
  // explicitly known not to have started the acknowledgement attempt.
  try {
    const auto &id = request_id.value();
    if (id.empty() || id.size() > kMaximumManagerApplyJournalIdentifierBytes ||
        std::any_of(id.begin(), id.end(), [](char c) {
          const auto byte = static_cast<unsigned char>(c);
          return byte < 0x21U || byte > 0x7eU;
        }))
      throw core::RequestInvalid("request_context.request_id", "invalid",
                                 "Acknowledgement request ID must be bounded printable ASCII.");
    if (source_label.empty() || source_label.size() > kMaximumManagerApplySourceLabelBytes ||
        std::any_of(source_label.begin(), source_label.end(), [](char c) {
          const auto byte = static_cast<unsigned char>(c);
          return byte < 0x20U || byte == 0x7fU;
        }))
      throw core::RequestInvalid("request_context.source_label", "invalid",
                                 "Acknowledgement source label is invalid.");
    status = apply_journal_.ReadStatus(apply_id, apply_generation);
    Require(status && (for_new ? FailedApplyNewReleaseEligible(*status)
                               : FailedApplyAcknowledgementEligible(*status)),
            for_new ? "failed Apply holds no fence New can release"
                    : "failed Apply is not eligible for before-effect acknowledgement");
    const auto previous = apply_journal_.ReadFailedApplyRelease(apply_id, apply_generation);
    Require(!previous, "failed Apply release was already attempted; inspect its outcome");

    const auto &intent = status->intent.intent;
    const auto &parent = intent.parent_authoring_claim;
    const auto claim_intent = parent ? parent->intent : DirectAdmissionIntent(status->intent);
    const core::CellAdmissionRecoveryReceiptQueryDTO claim_query{claim_intent, parent, std::nullopt};
    const auto claim = cell_admission_.GetCellAdmissionRecoveryReceipt(claim_query);
    if (!claim.ok()) FailCause(claim.error(), claim.error().message, kKernelProducer);
    Require(core::ValidateCellAdmissionRecoveryReceiptQuery(claim_query).ok() &&
                core::ValidateCellAdmissionRecoveryReceipt(claim.value()).ok() &&
                claim.value().status == core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
                claim.value().matched_claim && claim.value().matched_claim->intent == claim_intent &&
                (!parent || claim.value().matched_claim == parent),
            "failed Apply admission claim is not held exactly");

    const auto active = active_plan_source_.ReadCurrentPlan();
    const auto scene = scene_owner_.CurrentProjectionRevision();
    const auto &predecessor = intent.expected_active_projection_revision;
    Require(active.valid() && SameDeploymentRevision(
                active.active_publication.request.candidate_projection_revision, predecessor) &&
                scene.valid() && SameDeploymentRevision(scene, predecessor) &&
                scene.scene_revision.scene_id == predecessor.scene_revision.scene_id,
            "failed Apply predecessor is no longer current");
    const auto &accepted = status->fence_acceptance->accepted_fence;
    const core::ConfigurationFenceStatusQueryDTO query{
        {apply_id, apply_generation}, accepted.configuration_fence_id,
        accepted.configuration_fence_generation, accepted.captured_scope, manager_runtime_epoch_};
    const auto held = kernel_fence_.QueryStatus(query);
    const auto &evidence = held.reconciliation_evidence;
    Require(held.matches(query) && held.state == core::ConfigurationFenceState::kHeld &&
                held.admission_fenced &&
                SameDeploymentRevision(evidence.active_projection_revision, predecessor) &&
                evidence.active_projection_revision.scene_revision.scene_id ==
                    predecessor.scene_revision.scene_id &&
                evidence.active_publication_generation == active.active_publication.publication_generation &&
                evidence.active_public_pointer_digest ==
                    active.active_publication.durable_public_pointer_content_digest &&
                // New empties whatever the failed Apply changed (N7).
                (for_new || evidence.active_control_configurations ==
                    status->fence_attempt->fence_intent.expected_active_control_configurations),
            "failed Apply fence no longer proves the unchanged predecessor");
    release_request = core::ConfigurationFenceReleaseDTO{evidence, manager_runtime_epoch_};
    Require(release_request->valid(), "failed Apply fence still contains affected operations");
  } catch (const std::exception &error) {
    auto cause = core::CauseOf(error, "manager_command_rejected", kApplyProducer);
    cause.error.details["effect"] = "none";
    throw core::TypedError(cause.error, std::move(cause.producer));
  }
  const auto &request = *release_request;
  const auto &parent = status->intent.intent.parent_authoring_claim;
  // The exact held claim and fence are release conditions. Readiness is not:
  // ordinary liveness renewal recomputes it separately; a still-closed gate
  // remains closed until its own evidence permits admission.
  // Persist the one operator-authorized attempt before sending. If the call's
  // outcome is unknown the row remains incomplete and no resend is permitted.
  try {
    apply_journal_.BeginFailedApplyRelease(apply_id, apply_generation, request_id, source_label, request);
  } catch (const std::exception &error) {
    FailAcknowledgementUncertain(error, "record_attempt", kApplyProducer,
        "The acknowledgement record could not be confirmed. Inspect the current admission state and diagnose the outcome.");
  }
  std::optional<core::ConfigurationFenceReleasedDTO> released;
  try {
    released = kernel_fence_.Release(request);
    Require(released->valid() && released->request == request,
            "Kernel did not return the acknowledged fence release");
  } catch (const std::exception &error) {
    FailAcknowledgementUncertain(error, "fence_release", kKernelProducer,
        "The Apply hold release could not be confirmed. Inspect the current admission state and diagnose the outcome.");
  }
  try {
    apply_journal_.CompleteFailedApplyRelease(apply_id, apply_generation, *released);
  } catch (const std::exception &error) {
    FailAcknowledgementUncertain(error, "record_release", kApplyProducer,
        "The Apply hold was released, but its record could not be confirmed. Inspect the current admission state and diagnose the outcome.");
  }
  try {
    Require(ReleaseAdmission(status->intent, *status->terminal_outcome, cell_admission_,
                             parent),
            "failed Apply admission claim could not be settled");
  } catch (const std::exception &error) {
    FailAcknowledgementUncertain(error, "claim_release", kKernelProducer,
        "The Apply hold was released, but admission settlement could not be confirmed. Inspect the current admission state and diagnose the outcome.");
  }
  try {
    apply_journal_.CompleteFailedApplyAcknowledgement(apply_id, apply_generation);
  } catch (const std::exception &error) {
    FailAcknowledgementUncertain(error, "record_acknowledgement", kApplyProducer,
        "Admission was settled, but the acknowledgement record could not be confirmed. Inspect the current admission state and diagnose the outcome.");
  }
}

} // namespace manyforge::component_manager
