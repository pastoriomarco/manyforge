#include "manyforge_component_manager/selected_bundle_transition.hpp"
#include "manyforge_component_manager/selected_runtime_composition.hpp"

#include "selected_runtime_composition_rpc_internal.hpp"

#include <algorithm>
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

bool HostApplyRequest::valid() const noexcept {
  const auto sorted_unique = [](const auto &values) {
    return std::adjacent_find(values.begin(), values.end(),
                              [](const auto &lhs, const auto &rhs) {
                                return !(lhs < rhs);
                              }) == values.end() &&
           std::none_of(values.begin(), values.end(),
                        [](const auto &value) { return value.empty(); });
  };
  return apply.valid() && !candidate_retained_bundle_id.empty() &&
         !candidate_graph_revision.empty() && sorted_unique(components) &&
         sorted_unique(retained_components) &&
         sorted_unique(removed_components) && sorted_unique(removed_domains) &&
         std::includes(components.begin(), components.end(),
                       retained_components.begin(), retained_components.end()) &&
         std::none_of(removed_components.begin(), removed_components.end(),
                      [&](const auto &id) {
                        return std::binary_search(components.begin(),
                                                  components.end(), id);
                      });
}

HostApplyRequest MakeHostApplyRequest(
    const manyforge::core::NamedApplyPublicationTransactionDTO &apply,
    const manyforge::deployment::RetainedBundle &predecessor,
    const manyforge::deployment::RetainedBundle &candidate,
    const manyforge::deployment::ApplyComponentDeltaDTO &delta) {
  using manyforge::deployment::ApplyComponentDisposition;
  const auto &graph = candidate.value().cell_graph;
  HostApplyRequest request{apply, candidate.retained_bundle_id(),
                           graph.graph_revision, {}, {}, {}, {}};
  for (const auto &component : graph.controllable_components)
    request.components.push_back(component.controllable_component_id);
  for (const auto &change : delta.components) {
    if (change.disposition == ApplyComponentDisposition::kRetain)
      request.retained_components.push_back(change.controllable_component_id);
    else if (change.disposition == ApplyComponentDisposition::kRemove)
      request.removed_components.push_back(change.controllable_component_id);
  }
  for (const auto &domain : predecessor.value().cell_graph.control_domains) {
    if (std::none_of(graph.control_domains.begin(), graph.control_domains.end(),
                     [&](const auto &kept) {
                       return kept.control_domain_id == domain.control_domain_id;
                     }))
      request.removed_domains.push_back(domain.control_domain_id);
  }
  std::sort(request.components.begin(), request.components.end());
  std::sort(request.retained_components.begin(), request.retained_components.end());
  std::sort(request.removed_components.begin(), request.removed_components.end());
  std::sort(request.removed_domains.begin(), request.removed_domains.end());
  return request;
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
