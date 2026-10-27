#include "manyforge_component_manager/selected_bundle_transition.hpp"
#include "manyforge_component_manager/selected_runtime_composition.hpp"

#include "selected_runtime_composition_rpc_internal.hpp"

#include <algorithm>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <stdexcept>
#include <tuple>

namespace manyforge::component_manager {

bool ManagerComponentProcessStopRequestDTO::valid() const noexcept {
  using namespace manyforge::core;
  const auto identity = [](const auto &value) {
    return !value.empty() && value.value().size() <= kMaxLifecycleIdentityBytes;
  };
  return identity(request_context.request_id) && identity(request_context.request_owner_epoch) &&
         !request_context.source_label.empty() &&
         request_context.source_label.size() <= kMaxLifecycleRequestLabelBytes &&
         (!request_context.correlation_id ||
          (!request_context.correlation_id->empty() &&
           request_context.correlation_id->size() <= kMaxLifecycleRequestLabelBytes)) &&
         predecessor_projection_revision.valid() &&
         expected_predecessor_publication_generation.nonzero() &&
         IsExactLowercaseSha256Digest(expected_predecessor_public_pointer_digest.value()) &&
         identity(runtime_unit_id) && identity(runtime_epoch) && identity(manager_runtime_epoch) &&
         identity(controllable_component_id) && !launch_identity.empty() &&
         launch_identity.size() <= detail::kMaximumSelectedRuntimeCompositionRpcPacketBytes && leader_pid > 0;
}

bool ManagerComponentProcessStopResultDTO::valid() const noexcept {
  using namespace manyforge::core;
  if (!request.valid() ||
      !(state == ControllerLaunchCleanupState::kNotRequested ||
        state == ControllerLaunchCleanupState::kPending ||
        state == ControllerLaunchCleanupState::kUnknown ||
        state == ControllerLaunchCleanupState::kDrained)) return false;
  if (state != ControllerLaunchCleanupState::kDrained) return !receipt;
  return receipt && !error && receipt->leader_pid == request.leader_pid &&
         receipt->controllable_component_id == request.controllable_component_id.value() &&
         receipt->launch_identity == request.launch_identity && !receipt->observed_utc.empty() &&
         receipt->observed_utc.size() <= kMaxLifecycleIdentityBytes;
}

bool ManagerComponentProcessInventoryRequestDTO::valid() const noexcept {
  using namespace manyforge::core;
  const auto identity = [](const auto &value) {
    return !value.empty() && value.value().size() <= kMaxLifecycleIdentityBytes;
  };
  return predecessor_projection_revision.valid() &&
         expected_predecessor_publication_generation.nonzero() &&
         IsExactLowercaseSha256Digest(
             expected_predecessor_public_pointer_digest.value()) &&
         identity(runtime_unit_id) && identity(runtime_epoch) &&
         identity(manager_runtime_epoch);
}

bool ManagerPneumaticNoneProcessOwnerDTO::valid() const noexcept {
  const auto identity = [](const auto &value) {
    return !value.empty() &&
           value.value().size() <= manyforge::core::kMaxLifecycleIdentityBytes &&
           value.value().find('\0') == std::string::npos;
  };
  return identity(command_target_id) && identity(control_binding_id) &&
         identity(control_binding_revision);
}

bool ManagerComponentProcessOccurrenceDTO::valid() const noexcept {
  using namespace manyforge::core;
  if (process_owner_kind == ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal) {
    return !controllable_component_id.empty() &&
           controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes &&
           controllable_component_id.value().find('\0') == std::string::npos &&
           launch_identity.empty() && leader_pid == 0 &&
           process_presence == ProcessPresenceState::kAbsent &&
           cleanup_state == ControllerLaunchCleanupState::kNotRequested && !receipt &&
           installed_runtime_identity &&
           IsExactLowercaseSha256Digest(installed_runtime_identity->value()) &&
           pneumatic_none_owner && pneumatic_none_owner->valid();
  }
  if (process_owner_kind != ManagerComponentProcessOwnerKindDTO::kControllerLaunch ||
      pneumatic_none_owner) return false;
  if (controllable_component_id.empty() ||
      controllable_component_id.value().size() > kMaxLifecycleIdentityBytes ||
      launch_identity.empty() ||
      launch_identity.size() > detail::kMaximumSelectedRuntimeCompositionRpcPacketBytes || leader_pid <= 0 ||
      (process_presence != ProcessPresenceState::kUnknown &&
       process_presence != ProcessPresenceState::kAbsent &&
       process_presence != ProcessPresenceState::kPresent) ||
      (cleanup_state != ControllerLaunchCleanupState::kNotRequested &&
       cleanup_state != ControllerLaunchCleanupState::kPending &&
       cleanup_state != ControllerLaunchCleanupState::kUnknown &&
       cleanup_state != ControllerLaunchCleanupState::kDrained) ||
      (installed_runtime_identity &&
       !IsExactLowercaseSha256Digest(installed_runtime_identity->value())))
    return false;
  if (cleanup_state != ControllerLaunchCleanupState::kDrained)
    return !receipt;
  if (process_presence == ProcessPresenceState::kPresent)
    return false;
  return receipt && receipt->leader_pid == leader_pid &&
         receipt->controllable_component_id ==
             controllable_component_id.value() &&
         receipt->launch_identity == launch_identity &&
         !receipt->observed_utc.empty() &&
         receipt->observed_utc.size() <= kMaxLifecycleIdentityBytes;
}

bool ManagerComponentProcessInventoryResultDTO::valid() const noexcept {
  if (!request.valid() ||
      process_occurrences.size() > kMaximumComponentProcessInventoryEntries ||
      component_lifecycles.size() > kMaximumComponentProcessInventoryEntries ||
      !std::all_of(component_lifecycles.begin(), component_lifecycles.end(),
                   [](const auto &row) {
                     return !row.controllable_component_id.empty() &&
                            row.controllable_component_id.value().size() <=
                                manyforge::core::kMaxLifecycleIdentityBytes &&
                            row.controllable_component_id.value().find('\0') ==
                                std::string::npos &&
                            static_cast<unsigned>(row.state) <=
                                static_cast<unsigned>(
                                    manyforge::core::
                                        ComponentLifecycleStateDTO::kDisabled);
                   }) ||
      !std::all_of(process_occurrences.begin(), process_occurrences.end(),
                   [](const auto &row) { return row.valid(); }))
    return false;
  if (std::adjacent_find(
          component_lifecycles.begin(), component_lifecycles.end(),
          [](const auto &a, const auto &b) {
            return !(a.controllable_component_id < b.controllable_component_id);
          }) != component_lifecycles.end())
    return false;
  const auto key = [](const auto &row) {
    return std::tie(row.controllable_component_id, row.launch_identity,
                    row.leader_pid);
  };
  if (std::adjacent_find(process_occurrences.begin(), process_occurrences.end(),
                         [&](const auto &a, const auto &b) {
                           return !(key(a) < key(b));
                         }) != process_occurrences.end())
    return false;
  return !error || (!error->code.empty() && !error->message.empty());
}

bool ManagerComponentBringUpInspectionRequestDTO::valid() const noexcept {
  return candidate_projection_revision.valid() && predecessor_projection_revision.valid() &&
         manyforge::core::IsExactLowercaseSha256Digest(candidate_retained_bundle_digest.value()) &&
         candidate_retained_bundle_bytes != 0U &&
         expected_predecessor_publication_generation.nonzero() &&
         manyforge::core::IsExactLowercaseSha256Digest(expected_predecessor_public_pointer_digest.value()) &&
         !runtime_unit_id.empty() && !runtime_epoch.empty() && !manager_runtime_epoch.empty() &&
         (!planning_component_id || (!planning_component_id->empty() &&
          planning_component_id->value().size() <= manyforge::core::kMaxLifecycleIdentityBytes));
}

bool ManagerComponentProcessObservationDTO::valid() const noexcept {
  if (process_owner_kind == ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal) {
    return !controllable_component_id.empty() &&
           controllable_component_id.value().size() <= manyforge::core::kMaxLifecycleIdentityBytes &&
           controllable_component_id.value().find('\0') == std::string::npos &&
           process_presence == ProcessPresenceState::kAbsent &&
           pneumatic_none_owner && pneumatic_none_owner->valid() &&
           installed_runtime_identity &&
           manyforge::core::IsExactLowercaseSha256Digest(installed_runtime_identity->value());
  }
  if (process_owner_kind != ManagerComponentProcessOwnerKindDTO::kControllerLaunch ||
      pneumatic_none_owner || installed_runtime_identity) return false;
  return !controllable_component_id.empty() &&
         (process_presence == ProcessPresenceState::kUnknown ||
          process_presence == ProcessPresenceState::kAbsent ||
          process_presence == ProcessPresenceState::kPresent);
}

bool ManagerComponentBringUpPlanningFactsDTO::valid() const noexcept {
  using namespace manyforge::core;
  if (component_id.empty() || component_id.value().size() > kMaxLifecycleIdentityBytes ||
      owner_epoch.empty() || owner_epoch.value().size() > kMaxLifecycleIdentityBytes ||
      !generation_selection.locally_valid() ||
      !((requested_target == LifecycleTargetStateDTO::kActive &&
         (next_intent == ComponentLifecycleIntentKindDTO::kPrepare ||
          next_intent == ComponentLifecycleIntentKindDTO::kEnable)) ||
        (requested_target == LifecycleTargetStateDTO::kPrepared &&
         next_intent == ComponentLifecycleIntentKindDTO::kPrepare) ||
        (requested_target == LifecycleTargetStateDTO::kDisabled &&
         next_intent == ComponentLifecycleIntentKindDTO::kDisable)) ||
      candidate_expected_domains.empty() ||
      candidate_expected_domains.size() > kMaxLifecycleExpectedDomains) return false;
  std::size_t bindings = 0U;
  for (std::size_t index = 0U; index < candidate_expected_domains.size(); ++index) {
    const auto &domain = candidate_expected_domains[index];
    if (!domain.locally_valid() ||
        (index && !(candidate_expected_domains[index - 1U].control_domain_id <
                    domain.control_domain_id)) ||
        bindings >
            kMaxLifecycleExpectedBindings - domain.control_bindings().size())
      return false;
    bindings += domain.control_bindings().size();
  }
  return true;
}

bool ManagerComponentBringUpInspectionResultDTO::valid() const noexcept {
  if (!request.valid() ||
      !std::all_of(configured_components.begin(), configured_components.end(),
                   [](const auto &row) { return row.valid(); }) ||
      std::adjacent_find(configured_components.begin(), configured_components.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return !(lhs.controllable_component_id < rhs.controllable_component_id);
                         }) != configured_components.end()) return false;
  if (!planning_facts) return !request.planning_component_id;
  return request.planning_component_id && planning_facts->valid() &&
      planning_facts->component_id == *request.planning_component_id &&
      std::any_of(configured_components.begin(), configured_components.end(), [&](const auto &row) {
        return row.controllable_component_id == planning_facts->component_id;
      });
}

bool ManagerSelectedRuntimeCompositionRequestDTO::valid() const noexcept {
  return (phase == ManagerSelectedRuntimeCompositionPhase::kPrepare ||
          phase == ManagerSelectedRuntimeCompositionPhase::kFinalize) &&
         manyforge::core::IsExactLowercaseSha256Digest(
             authoritative_scene_snapshot_digest.value()) &&
         !apply_id.empty() && apply_generation.nonzero() &&
         candidate_projection_revision.valid() &&
         expected_predecessor_publication_generation.nonzero() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             expected_predecessor_public_pointer_digest.value()) &&
         manyforge::core::IsExactLowercaseSha256Digest(
             retained_bundle_canonical_content_digest.value()) &&
         retained_bundle_canonical_byte_size != 0U &&
         !runtime_unit_id.empty() && !runtime_epoch.empty() &&
         !manager_runtime_epoch.empty() && !configuration_fence_id.empty() &&
         configuration_fence_generation.nonzero() &&
         configuration_fence_scope.valid() && component_delta.valid() &&
         manyforge::deployment::apply_component_delta_is_fenced(
             component_delta, configuration_fence_scope) &&
         predecessor_projection_revision.valid();
}

bool ManagerRuntimeComponentReceiptDTO::valid() const noexcept {
  using manyforge::deployment::ApplyComponentDisposition;
  const auto epochs_sorted = std::adjacent_find(
      runtime_unit_epochs.begin(), runtime_unit_epochs.end(),
      [](const auto &lhs, const auto &rhs) {
        return !(lhs.runtime_unit_id < rhs.runtime_unit_id);
      }) == runtime_unit_epochs.end();
  return !controllable_component_id.empty() &&
         disposition != ApplyComponentDisposition::kRemove &&
         installed_runtime_identity.has_value() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             installed_runtime_identity->value()) &&
         epochs_sorted &&
         std::all_of(runtime_unit_epochs.begin(), runtime_unit_epochs.end(),
                     [](const auto &epoch) {
                       return !epoch.runtime_unit_id.empty() &&
                              !epoch.runtime_epoch.empty();
                     }) &&
         (predecessor_runtime_retained || !product_binding_resealed);
}

bool ComponentReceiptsMatchRequest(
    const ManagerSelectedRuntimeCompositionRequestDTO &request,
    const std::vector<ManagerRuntimeComponentReceiptDTO> &receipts) noexcept {
  using manyforge::deployment::ApplyComponentDisposition;
  const auto &delta = request.component_delta;
  if (!request.valid()) return false;
  // Bindings are sealed to one bundle and graph revision; a retained
  // controller is re-sealed exactly when that product identity changes.
  const bool product_changed =
      request.predecessor_projection_revision.retained_bundle_id !=
          request.candidate_projection_revision.retained_bundle_id ||
      request.predecessor_projection_revision.graph_revision !=
          request.candidate_projection_revision.graph_revision;
  std::size_t next = 0U;
  for (const auto &change : delta.components) {
    if (change.disposition == ApplyComponentDisposition::kRemove) continue;
    if (next == receipts.size()) return false;
    const auto &receipt = receipts[next++];
    if (!receipt.valid() ||
        receipt.controllable_component_id != change.controllable_component_id ||
        receipt.disposition != change.disposition ||
        receipt.installed_runtime_identity !=
            change.candidate_runtime_identity ||
        // The sole managed native host owns every component runtime (N7):
        // each receipt names exactly that runtime unit at the request's
        // epoch, never a stale, foreign or additional owner.
        receipt.runtime_unit_epochs !=
            std::vector<manyforge::core::RuntimeUnitEpochBindingDTO>{
                {request.runtime_unit_id, request.runtime_epoch}} ||
        (receipt.predecessor_runtime_retained &&
         receipt.product_binding_resealed != product_changed)) {
      return false;
    }
  }
  return next == receipts.size();
}

bool ManagerSelectedRuntimeCompositionResultDTO::valid() const noexcept {
  return request.valid() &&
         (state == ManagerSelectedRuntimeCompositionState::kPreparing ||
          state == ManagerSelectedRuntimeCompositionState::kInstalled) &&
         (state == ManagerSelectedRuntimeCompositionState::kInstalled ||
          component_receipts.empty()) &&
         std::all_of(component_receipts.begin(), component_receipts.end(),
                     [](const auto &receipt) { return receipt.valid(); }) &&
         std::adjacent_find(component_receipts.begin(), component_receipts.end(),
                            [](const auto &lhs, const auto &rhs) {
                              return !(lhs.controllable_component_id <
                                       rhs.controllable_component_id);
                            }) == component_receipts.end();
}

bool ManagerSelectedComponentBringUpRequestDTO::valid() const noexcept {
  return intent.locally_valid() &&
         intent.intent ==
             manyforge::core::ComponentLifecycleIntentKindDTO::kEnable &&
         !intent.program_node_evidence.has_value() &&
         candidate_projection_revision.valid() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             candidate_retained_bundle_digest.value()) &&
         candidate_retained_bundle_bytes > 0U && !runtime_unit_id.empty() &&
         !runtime_epoch.empty() && !manager_runtime_epoch.empty();
}

bool ManagerSelectedComponentBringUpReadRequestDTO::valid() const noexcept {
  return request.valid();
}

bool ManagerSelectedComponentBringUpResultDTO::valid() const {
  if (!request.valid())
    return false;
  if (!result.has_value())
    return true;
  if (!result->locally_valid() || result->incident_recovery.has_value())
    return false;
  if (!result->status.has_value())
    return true;
  const auto &status = *result->status;
  if (!status.selected_bring_up.has_value() ||
      !status.accepted_intent.replay_equal(request.intent) ||
      result->acceptance->request_id !=
          request.intent.request_context.request_id) {
    return false;
  }
  const auto &selected = *status.selected_bring_up;
  return selected.candidate_projection_revision ==
             request.candidate_projection_revision &&
         selected.candidate_retained_bundle_digest ==
             request.candidate_retained_bundle_digest &&
         selected.candidate_retained_bundle_bytes ==
             request.candidate_retained_bundle_bytes &&
         selected.original_request_payload_digest ==
             manyforge::core::SelectedComponentBringUpSemanticRequestDigest(
                 request.intent, request.candidate_projection_revision,
                 request.candidate_retained_bundle_digest,
                 request.candidate_retained_bundle_bytes,
                 request.runtime_unit_id, request.runtime_epoch,
                 request.manager_runtime_epoch);
}

bool ManagerApplyComponentBringUpRequestDTO::valid() const noexcept {
  using manyforge::core::ComponentLifecycleIntentKindDTO;
  return intent.locally_valid() &&
         (intent.intent == ComponentLifecycleIntentKindDTO::kPrepare ||
          intent.intent == ComponentLifecycleIntentKindDTO::kEnable ||
          intent.intent == ComponentLifecycleIntentKindDTO::kDisable) &&
         !intent.program_node_evidence.has_value() &&
         candidate_projection_revision.valid() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             candidate_retained_bundle_digest.value()) &&
         candidate_retained_bundle_bytes > 0U && !runtime_unit_id.empty() &&
         !runtime_epoch.empty() && !manager_runtime_epoch.empty() &&
         parent_authority.valid() &&
         parent_authority.held_fence_query.manager_runtime_epoch ==
             manager_runtime_epoch;
}

bool ManagerApplyComponentBringUpReadRequestDTO::valid() const noexcept {
  return request.valid();
}

bool ManagerApplyComponentBringUpResultDTO::valid() const {
  if (!request.valid()) return false;
  if (!result.has_value()) return true;
  if (!result->locally_valid() || result->incident_recovery.has_value())
    return false;
  if (!result->status.has_value()) return true;
  const auto &status = *result->status;
  if (!status.apply_bring_up.has_value() ||
      !status.accepted_intent.replay_equal(request.intent) ||
      result->acceptance->request_id != request.intent.request_context.request_id)
    return false;
  const auto &apply = *status.apply_bring_up;
  return apply.parent_authority == request.parent_authority &&
         apply.candidate_projection_revision == request.candidate_projection_revision &&
         apply.candidate_retained_bundle_digest == request.candidate_retained_bundle_digest &&
         apply.candidate_retained_bundle_bytes == request.candidate_retained_bundle_bytes &&
         apply.original_execution_host == manyforge::core::RuntimeUnitEpochBindingDTO{
             request.runtime_unit_id, request.runtime_epoch} &&
         apply.original_request_payload_digest ==
             manyforge::core::ApplyDelegatedComponentLifecycleSemanticRequestDigest(
                 request.intent, request.candidate_projection_revision,
                 request.candidate_retained_bundle_digest,
                 request.candidate_retained_bundle_bytes, request.runtime_unit_id,
                 request.runtime_epoch, request.manager_runtime_epoch,
                 request.parent_authority.parent);
}

bool ManagerApplyPrivateMaterializationDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             authoritative_scene_snapshot_digest.value()) &&
         !apply_id.empty() && apply_generation.nonzero() &&
         candidate_projection_revision.valid() &&
         manyforge::core::IsExactLowercaseSha256Digest(
             retained_bundle_canonical_content_digest.value()) &&
         retained_bundle_canonical_byte_size > 0U;
}

bool ManagerApplyPublicationReceiptDTO::valid() const noexcept {
  return manyforge::core::IsExactLowercaseSha256Digest(
             private_materialization_content_digest.value()) &&
         publication_receipt.valid() &&
         publication_receipt.request
                 .private_materialization_content_digest ==
             private_materialization_content_digest;
}

} // namespace manyforge::component_manager
