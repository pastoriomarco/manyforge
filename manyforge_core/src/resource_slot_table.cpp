#include <algorithm>
#include <limits>
#include <manyforge_core/resource_slot_table.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] Result<void> reject(std::string message, std::string reason,
                                  ErrorCode code = ErrorCode::kExecutionRejected) {
  return Result<void>::Err(
      ErrorDTO{std::string(to_string(code)), std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool legal_transition(ResourceSlotState current, ResourceSlotState next) noexcept {
  switch (current) {
    case ResourceSlotState::kReserved:
      return next == ResourceSlotState::kDispatched || next == ResourceSlotState::kStopping ||
             next == ResourceSlotState::kTerminal;
    case ResourceSlotState::kDispatched:
      return next == ResourceSlotState::kStopping || next == ResourceSlotState::kTerminal;
    case ResourceSlotState::kStopping:
      return next == ResourceSlotState::kTerminal;
    case ResourceSlotState::kFree:
    case ResourceSlotState::kTerminal:
    case ResourceSlotState::kUnreconciledExternalOrOrphanGoal:
      return false;
  }
  return false;
}

[[nodiscard]] bool canonical_active_configurations(
    const std::vector<DomainActiveControlConfigurationDTO>& configurations) {
  return std::all_of(configurations.begin(), configurations.end(),
                     [](const auto& row) { return row.valid(); }) &&
         std::is_sorted(configurations.begin(), configurations.end(),
                        [](const auto& left, const auto& right) {
                          return left.control_domain_id < right.control_domain_id;
                        }) &&
         std::adjacent_find(configurations.begin(), configurations.end(),
                            [](const auto& left, const auto& right) {
                              return left.control_domain_id == right.control_domain_id;
                            }) == configurations.end();
}

[[nodiscard]] auto stop_fence_owner_key(const ResourceSlotStopFenceOwnerDTO& row) noexcept {
  return std::tie(row.expected_owner.operation_id, row.expected_owner.participant_id);
}

[[nodiscard]] auto reservation_owner_key(const ResourceSlotReservationOwnerDTO& row) noexcept {
  return std::tie(row.owner.operation_id, row.owner.participant_id);
}

template <typename Value>
[[nodiscard]] Result<Value> reject_value(std::string message, std::string reason,
                                         ErrorCode code = ErrorCode::kExecutionRejected) {
  return Result<Value>::Err(
      ErrorDTO{std::string(to_string(code)), std::move(message), {{"reason", std::move(reason)}}});
}

}  // namespace

bool ResourceSlotOwnerDTO::valid() const noexcept {
  return !operation_id.empty() && operation_revision.nonzero() && !participant_id.empty() &&
         !control_domain_id.empty() && !control_domain_revision.empty() &&
         !control_binding_id.empty() && !control_binding_revision.empty() &&
         !controller_topology_id.empty() && !topology_revision.empty() &&
         !control_route_id.empty() && !active_control_configuration_id.empty() &&
         active_control_configuration_generation.nonzero();
}

ResourceSlotTable::ResourceSlotTable(
    std::vector<ResourceId> resources,
    std::vector<DomainActiveControlConfigurationDTO> active_control_configurations,
    std::shared_ptr<std::recursive_mutex> admission_mutex)
    : admission_mutex_(admission_mutex != nullptr ? std::move(admission_mutex)
                                                  : std::make_shared<std::recursive_mutex>()),
      active_control_configurations_(std::move(active_control_configurations)) {
  if (resources.empty() || resources.size() > kMaxKernelResourceSlots) {
    throw std::invalid_argument("Kernel resource inventory is empty or exceeds its fixed bound");
  }
  std::sort(resources.begin(), resources.end());
  if (std::any_of(resources.begin(), resources.end(),
                  [](const ResourceId& id) { return id.empty(); }) ||
      std::adjacent_find(resources.begin(), resources.end()) != resources.end()) {
    throw std::invalid_argument(
        "Kernel resource inventory contains an empty or duplicate identity");
  }
  if (!canonical_active_configurations(active_control_configurations_)) {
    throw std::invalid_argument("Kernel active-control-configuration inventory is noncanonical");
  }
  slots_.reserve(resources.size());
  for (auto& resource : resources) {
    slots_.push_back(ResourceSlotSnapshotDTO{std::move(resource), ResourceSlotState::kFree,
                                             std::nullopt, StopGeneration(0U)});
  }
}

Result<void> ResourceSlotTable::validate_resource_set_locked(
    const std::vector<ResourceId>& resources, std::vector<std::size_t>& indices) const {
  if (resources.empty() || resources.size() > slots_.size()) {
    return reject("resource scope is empty or exceeds the inventory",
                  "resource_scope_count_invalid", ErrorCode::kInvalidRequest);
  }
  std::set<ResourceId> unique;
  indices.clear();
  indices.reserve(resources.size());
  for (const auto& resource : resources) {
    if (resource.empty() || !unique.insert(resource).second) {
      return reject("resource scope contains an empty or duplicate identity",
                    "resource_scope_identity_invalid", ErrorCode::kInvalidRequest);
    }
    const auto iterator =
        std::lower_bound(slots_.begin(), slots_.end(), resource,
                         [](const ResourceSlotSnapshotDTO& slot, const ResourceId& id) {
                           return slot.resource_id < id;
                         });
    if (iterator == slots_.end() || iterator->resource_id != resource) {
      return reject("resource scope names an unknown resource", "resource_scope_unknown",
                    ErrorCode::kInvalidRequest);
    }
    indices.push_back(static_cast<std::size_t>(std::distance(slots_.begin(), iterator)));
  }
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::require_generation_available_locked() const {
  if (operation_resource_generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return reject("operation-resource generation is exhausted",
                  "operation_resource_generation_exhausted", ErrorCode::kInternalError);
  }
  return Result<void>::Ok();
}

void ResourceSlotTable::advance_generation_locked() noexcept {
  ++operation_resource_generation_;
}

Result<void> ResourceSlotTable::reserve(const ResourceSlotOwnerDTO& owner,
                                        const std::vector<ResourceId>& resources) {
  if (!owner.valid()) {
    return reject("resource reservation owner is invalid", "resource_owner_invalid",
                  ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(*admission_mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  for (const auto index : indices) {
    if (slots_[index].state != ResourceSlotState::kFree) {
      return reject("one or more resources are not free", "resource_scope_busy");
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return generation;
  }
  for (const auto index : indices) {
    slots_[index].state = ResourceSlotState::kReserved;
    slots_[index].owner = owner;
  }
  advance_generation_locked();
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::reserve_batch(
    const std::vector<ResourceSlotReservationOwnerDTO>& owners) {
  if (owners.empty() || owners.size() > slots_.size()) {
    return reject("coordinated resource reservation owner count is invalid",
                  "resource_reservation_owner_count_invalid", ErrorCode::kInvalidRequest);
  }
  if (!std::is_sorted(owners.begin(), owners.end(),
                      [](const auto& left, const auto& right) {
                        return reservation_owner_key(left) < reservation_owner_key(right);
                      }) ||
      std::adjacent_find(owners.begin(), owners.end(), [](const auto& left, const auto& right) {
        return reservation_owner_key(left) == reservation_owner_key(right);
      }) != owners.end()) {
    return reject("coordinated resource reservation owners are not canonical",
                  "resource_reservation_not_canonical", ErrorCode::kInvalidRequest);
  }

  std::lock_guard lock(*admission_mutex_);
  std::vector<std::pair<std::size_t, const ResourceSlotOwnerDTO*>> assignments;
  assignments.reserve(slots_.size());
  std::set<ResourceId> complete_resources;
  std::optional<std::pair<OperationId, OperationRevision>> operation_revision;
  std::vector<std::size_t> indices;
  for (const auto& row : owners) {
    if (!row.owner.valid() || row.resources.empty() ||
        !std::is_sorted(row.resources.begin(), row.resources.end()) ||
        std::adjacent_find(row.resources.begin(), row.resources.end()) != row.resources.end()) {
      return reject("coordinated resource reservation row is invalid",
                    "resource_reservation_row_invalid", ErrorCode::kInvalidRequest);
    }
    if (!operation_revision.has_value() || operation_revision->first != row.owner.operation_id) {
      operation_revision = std::pair(row.owner.operation_id, row.owner.operation_revision);
    } else if (operation_revision->second != row.owner.operation_revision) {
      return reject("participant owners disagree on their operation revision",
                    "resource_reservation_operation_revision_inconsistent",
                    ErrorCode::kInvalidRequest);
    }
    auto validation = validate_resource_set_locked(row.resources, indices);
    if (!validation.ok()) {
      return validation;
    }
    for (std::size_t offset = 0U; offset < row.resources.size(); ++offset) {
      if (!complete_resources.insert(row.resources[offset]).second) {
        return reject("coordinated resource reservation scopes overlap",
                      "resource_reservation_scope_overlap", ErrorCode::kInvalidRequest);
      }
      const auto index = indices[offset];
      if (slots_[index].state != ResourceSlotState::kFree) {
        return reject("one or more resources are not free", "resource_scope_busy");
      }
      assignments.emplace_back(index, &row.owner);
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return generation;
  }
  for (const auto& [index, owner] : assignments) {
    slots_[index].state = ResourceSlotState::kReserved;
    slots_[index].owner = *owner;
  }
  advance_generation_locked();
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::transition(const ResourceSlotOwnerDTO& owner,
                                           const std::vector<ResourceId>& resources,
                                           ResourceSlotState next_state) {
  if (!owner.valid()) {
    return reject("resource transition owner is invalid", "resource_owner_invalid",
                  ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(*admission_mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  for (const auto index : indices) {
    const auto& slot = slots_[index];
    if (!slot.owner.has_value() || *slot.owner != owner) {
      return reject("resource transition owner does not match the slot", "resource_owner_mismatch");
    }
    if (!legal_transition(slot.state, next_state)) {
      return reject("resource transition is illegal", "resource_transition_illegal",
                    ErrorCode::kInvalidStateTransition);
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return generation;
  }
  for (const auto index : indices) {
    slots_[index].state = next_state;
  }
  advance_generation_locked();
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::transition_batch(
    const std::vector<ResourceSlotReservationOwnerDTO>& owners, ResourceSlotState next_state) {
  if (owners.empty() || owners.size() > slots_.size() ||
      !std::is_sorted(owners.begin(), owners.end(),
                      [](const auto& left, const auto& right) {
                        return reservation_owner_key(left) < reservation_owner_key(right);
                      }) ||
      std::adjacent_find(owners.begin(), owners.end(), [](const auto& left, const auto& right) {
        return reservation_owner_key(left) == reservation_owner_key(right);
      }) != owners.end()) {
    return reject("coordinated resource transition owners are not canonical",
                  "resource_transition_batch_not_canonical", ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(*admission_mutex_);
  std::vector<std::size_t> assignments;
  assignments.reserve(slots_.size());
  std::set<ResourceId> complete_resources;
  std::vector<std::size_t> indices;
  for (const auto& row : owners) {
    if (!row.owner.valid() || row.resources.empty() ||
        !std::is_sorted(row.resources.begin(), row.resources.end()) ||
        std::adjacent_find(row.resources.begin(), row.resources.end()) != row.resources.end()) {
      return reject("coordinated resource transition row is invalid",
                    "resource_transition_batch_row_invalid", ErrorCode::kInvalidRequest);
    }
    auto validation = validate_resource_set_locked(row.resources, indices);
    if (!validation.ok()) {
      return validation;
    }
    for (std::size_t offset = 0U; offset < row.resources.size(); ++offset) {
      if (!complete_resources.insert(row.resources[offset]).second) {
        return reject("coordinated resource transition scopes overlap",
                      "resource_transition_batch_scope_overlap", ErrorCode::kInvalidRequest);
      }
      const auto index = indices[offset];
      const auto& slot = slots_[index];
      if (!slot.owner.has_value() || *slot.owner != row.owner) {
        return reject("resource transition owner does not match the slot",
                      "resource_owner_mismatch");
      }
      if (!legal_transition(slot.state, next_state)) {
        return reject("resource transition is illegal", "resource_transition_illegal",
                      ErrorCode::kInvalidStateTransition);
      }
      assignments.push_back(index);
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return generation;
  }
  for (const auto index : assignments) {
    slots_[index].state = next_state;
  }
  advance_generation_locked();
  return Result<void>::Ok();
}

Result<ResourceSlotOwnerDTO> ResourceSlotTable::revise_owner(
    const ResourceSlotOwnerDTO& owner, const std::vector<ResourceId>& resources,
    OperationRevision next_operation_revision) {
  if (!owner.valid() || !next_operation_revision.nonzero() ||
      next_operation_revision <= owner.operation_revision) {
    return Result<ResourceSlotOwnerDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "resource owner revision must advance monotonically",
                 {{"reason", "resource_owner_revision_invalid"}}});
  }
  std::lock_guard lock(*admission_mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return Result<ResourceSlotOwnerDTO>::Err(validation.error());
  }
  for (const auto index : indices) {
    const auto& slot = slots_[index];
    if (!slot.owner.has_value() || *slot.owner != owner) {
      return Result<ResourceSlotOwnerDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                   "resource owner revision names a stale or mismatched slot",
                   {{"reason", "resource_owner_mismatch"}}});
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return Result<ResourceSlotOwnerDTO>::Err(generation.error());
  }
  auto revised = owner;
  revised.operation_revision = next_operation_revision;
  for (const auto index : indices) {
    slots_[index].owner = revised;
  }
  advance_generation_locked();
  return Result<ResourceSlotOwnerDTO>::Ok(std::move(revised));
}

Result<std::vector<ResourceSlotOwnerDTO>> ResourceSlotTable::revise_owner_batch(
    const std::vector<ResourceSlotReservationOwnerDTO>& owners,
    OperationRevision next_operation_revision) {
  std::lock_guard lock(*admission_mutex_);
  return revise_owner_batch_locked(owners, std::nullopt, next_operation_revision);
}

Result<std::vector<ResourceSlotOwnerDTO>> ResourceSlotTable::revise_owner_batch_locked(
    const std::vector<ResourceSlotReservationOwnerDTO>& owners,
    std::optional<ResourceSlotState> next_state, OperationRevision next_operation_revision) {
  if (owners.empty() || owners.size() > slots_.size() || !next_operation_revision.nonzero() ||
      !std::is_sorted(owners.begin(), owners.end(),
                      [](const auto& left, const auto& right) {
                        return reservation_owner_key(left) < reservation_owner_key(right);
                      }) ||
      std::adjacent_find(owners.begin(), owners.end(), [](const auto& left, const auto& right) {
        return reservation_owner_key(left) == reservation_owner_key(right);
      }) != owners.end()) {
    return reject_value<std::vector<ResourceSlotOwnerDTO>>(
        "coordinated resource revision owners are not canonical",
        "resource_revision_batch_not_canonical", ErrorCode::kInvalidRequest);
  }
  const auto& first_owner = owners.front().owner;
  if (!first_owner.valid() || next_operation_revision <= first_owner.operation_revision) {
    return reject_value<std::vector<ResourceSlotOwnerDTO>>(
        "coordinated resource revision does not advance one valid operation",
        "resource_revision_batch_operation_invalid", ErrorCode::kInvalidRequest);
  }

  std::vector<std::vector<std::size_t>> assignments;
  assignments.reserve(owners.size());
  std::vector<ResourceSlotOwnerDTO> revised_owners;
  revised_owners.reserve(owners.size());
  std::set<ResourceId> complete_resources;
  std::vector<std::size_t> indices;
  for (const auto& row : owners) {
    if (!row.owner.valid() || row.owner.operation_id != first_owner.operation_id ||
        row.owner.operation_revision != first_owner.operation_revision ||
        next_operation_revision <= row.owner.operation_revision || row.resources.empty() ||
        !std::is_sorted(row.resources.begin(), row.resources.end()) ||
        std::adjacent_find(row.resources.begin(), row.resources.end()) != row.resources.end()) {
      return reject_value<std::vector<ResourceSlotOwnerDTO>>(
          "coordinated resource revision row is invalid or names another operation revision",
          "resource_revision_batch_row_invalid", ErrorCode::kInvalidRequest);
    }
    auto validation = validate_resource_set_locked(row.resources, indices);
    if (!validation.ok()) {
      return Result<std::vector<ResourceSlotOwnerDTO>>::Err(validation.error());
    }
    for (std::size_t offset = 0U; offset < row.resources.size(); ++offset) {
      if (!complete_resources.insert(row.resources[offset]).second) {
        return reject_value<std::vector<ResourceSlotOwnerDTO>>(
            "coordinated resource revision scopes overlap", "resource_revision_batch_scope_overlap",
            ErrorCode::kInvalidRequest);
      }
      const auto& slot = slots_[indices[offset]];
      if (!slot.owner.has_value() || *slot.owner != row.owner) {
        return reject_value<std::vector<ResourceSlotOwnerDTO>>(
            "coordinated resource revision owner does not match the slot",
            "resource_owner_mismatch");
      }
      if (next_state.has_value() && !legal_transition(slot.state, *next_state)) {
        return reject_value<std::vector<ResourceSlotOwnerDTO>>(
            "coordinated resource revision transition is illegal", "resource_transition_illegal",
            ErrorCode::kInvalidStateTransition);
      }
    }
    assignments.push_back(indices);
    auto revised = row.owner;
    revised.operation_revision = next_operation_revision;
    revised_owners.push_back(std::move(revised));
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return Result<std::vector<ResourceSlotOwnerDTO>>::Err(generation.error());
  }
  for (std::size_t owner_index = 0U; owner_index < assignments.size(); ++owner_index) {
    for (const auto slot_index : assignments[owner_index]) {
      slots_[slot_index].owner = revised_owners[owner_index];
      if (next_state.has_value()) {
        slots_[slot_index].state = *next_state;
      }
    }
  }
  advance_generation_locked();
  return Result<std::vector<ResourceSlotOwnerDTO>>::Ok(std::move(revised_owners));
}

Result<ResourceSlotOwnerDTO> ResourceSlotTable::transition_and_revise_owner(
    const ResourceSlotOwnerDTO& owner, const std::vector<ResourceId>& resources,
    ResourceSlotState next_state, OperationRevision next_operation_revision) {
  if (!owner.valid() || !next_operation_revision.nonzero() ||
      next_operation_revision <= owner.operation_revision) {
    return Result<ResourceSlotOwnerDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "resource owner revision must advance monotonically",
                 {{"reason", "resource_owner_revision_invalid"}}});
  }
  std::lock_guard lock(*admission_mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return Result<ResourceSlotOwnerDTO>::Err(validation.error());
  }
  for (const auto index : indices) {
    const auto& slot = slots_[index];
    if (!slot.owner.has_value() || *slot.owner != owner) {
      return Result<ResourceSlotOwnerDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kExecutionRejected)),
                   "resource transition owner does not match the slot",
                   {{"reason", "resource_owner_mismatch"}}});
    }
    if (!legal_transition(slot.state, next_state)) {
      return Result<ResourceSlotOwnerDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                   "resource transition is illegal",
                   {{"reason", "resource_transition_illegal"}}});
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return Result<ResourceSlotOwnerDTO>::Err(generation.error());
  }
  auto revised = owner;
  revised.operation_revision = next_operation_revision;
  for (const auto index : indices) {
    slots_[index].state = next_state;
    slots_[index].owner = revised;
  }
  advance_generation_locked();
  return Result<ResourceSlotOwnerDTO>::Ok(std::move(revised));
}

Result<std::vector<ResourceSlotOwnerDTO>> ResourceSlotTable::transition_and_revise_owner_batch(
    const std::vector<ResourceSlotReservationOwnerDTO>& owners, ResourceSlotState next_state,
    OperationRevision next_operation_revision) {
  std::lock_guard lock(*admission_mutex_);
  return revise_owner_batch_locked(owners, next_state, next_operation_revision);
}

Result<ResourceSlotStopFenceResultDTO> ResourceSlotTable::stop_fence_batch(
    const ResourceSlotStopFenceRequestDTO& request) {
  if (!request.stop_generation.nonzero() || request.owners.size() > kMaxKernelResourceSlots ||
      request.orphan_resources.size() > kMaxKernelResourceSlots ||
      (request.owners.empty() && request.orphan_resources.empty())) {
    return reject_value<ResourceSlotStopFenceResultDTO>(
        "batch Stop fence has an invalid generation or scope count",
        "resource_stop_fence_shape_invalid", ErrorCode::kInvalidRequest);
  }
  if (!std::is_sorted(request.owners.begin(), request.owners.end(),
                      [](const auto& left, const auto& right) {
                        return stop_fence_owner_key(left) < stop_fence_owner_key(right);
                      }) ||
      std::adjacent_find(request.owners.begin(), request.owners.end(),
                         [](const auto& left, const auto& right) {
                           return stop_fence_owner_key(left) == stop_fence_owner_key(right);
                         }) != request.owners.end() ||
      !std::is_sorted(request.orphan_resources.begin(), request.orphan_resources.end()) ||
      std::adjacent_find(request.orphan_resources.begin(), request.orphan_resources.end()) !=
          request.orphan_resources.end()) {
    return reject_value<ResourceSlotStopFenceResultDTO>("batch Stop fence rows are not canonical",
                                                        "resource_stop_fence_not_canonical",
                                                        ErrorCode::kInvalidRequest);
  }

  std::size_t resource_count = request.orphan_resources.size();
  std::set<ResourceId> unique_resources(request.orphan_resources.begin(),
                                        request.orphan_resources.end());
  if (unique_resources.size() != request.orphan_resources.size() ||
      std::any_of(request.orphan_resources.begin(), request.orphan_resources.end(),
                  [](const auto& resource) { return resource.empty(); })) {
    return reject_value<ResourceSlotStopFenceResultDTO>("batch Stop fence orphan scope is invalid",
                                                        "resource_stop_fence_scope_invalid",
                                                        ErrorCode::kInvalidRequest);
  }
  for (std::size_t index = 0U; index < request.owners.size(); ++index) {
    const auto& row = request.owners[index];
    if (!row.expected_owner.valid() || !row.next_operation_revision.nonzero() ||
        row.next_operation_revision <= row.expected_owner.operation_revision ||
        row.resources.empty() || row.resources.size() > kMaxKernelResourceSlots ||
        !std::is_sorted(row.resources.begin(), row.resources.end()) ||
        std::adjacent_find(row.resources.begin(), row.resources.end()) != row.resources.end() ||
        std::any_of(row.resources.begin(), row.resources.end(),
                    [](const auto& resource) { return resource.empty(); }) ||
        row.resources.size() > kMaxKernelResourceSlots - resource_count) {
      return reject_value<ResourceSlotStopFenceResultDTO>(
          "batch Stop fence owner scope or revision is invalid",
          "resource_stop_fence_owner_invalid", ErrorCode::kInvalidRequest);
    }
    resource_count += row.resources.size();
    for (const auto& resource : row.resources) {
      if (!unique_resources.insert(resource).second) {
        return reject_value<ResourceSlotStopFenceResultDTO>(
            "batch Stop fence resource scopes overlap", "resource_stop_fence_scope_overlap",
            ErrorCode::kInvalidRequest);
      }
    }
    if (index != 0U &&
        request.owners[index - 1U].expected_owner.operation_id == row.expected_owner.operation_id &&
        (request.owners[index - 1U].expected_owner.operation_revision !=
             row.expected_owner.operation_revision ||
         request.owners[index - 1U].next_operation_revision != row.next_operation_revision)) {
      return reject_value<ResourceSlotStopFenceResultDTO>(
          "one operation has inconsistent owner revisions in the batch Stop fence",
          "resource_stop_fence_operation_revision_inconsistent", ErrorCode::kInvalidRequest);
    }
  }

  std::lock_guard lock(*admission_mutex_);
  if (request.expected_operation_resource_generation != operation_resource_generation_) {
    return reject_value<ResourceSlotStopFenceResultDTO>(
        "batch Stop fence names a stale operation-resource generation",
        "operation_resource_generation_stale");
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return Result<ResourceSlotStopFenceResultDTO>::Err(generation.error());
  }

  std::vector<std::vector<std::size_t>> owner_indices;
  owner_indices.reserve(request.owners.size());
  for (const auto& row : request.owners) {
    owner_indices.emplace_back();
    auto validation = validate_resource_set_locked(row.resources, owner_indices.back());
    if (!validation.ok()) {
      return Result<ResourceSlotStopFenceResultDTO>::Err(validation.error());
    }
    for (const auto index : owner_indices.back()) {
      const auto& slot = slots_[index];
      if (!slot.owner.has_value() || *slot.owner != row.expected_owner) {
        return reject_value<ResourceSlotStopFenceResultDTO>(
            "batch Stop fence owner does not match the resource slot", "resource_owner_mismatch");
      }
      if (slot.state != ResourceSlotState::kReserved &&
          slot.state != ResourceSlotState::kDispatched) {
        return reject_value<ResourceSlotStopFenceResultDTO>(
            "batch Stop fence owner is not queued or dispatched",
            "resource_stop_fence_state_invalid", ErrorCode::kInvalidStateTransition);
      }
      if (request.stop_generation <= slot.stop_generation) {
        return reject_value<ResourceSlotStopFenceResultDTO>(
            "batch Stop generation does not advance the selected resource",
            "resource_stop_generation_stale");
      }
    }
  }

  std::vector<std::size_t> orphan_indices;
  if (!request.orphan_resources.empty()) {
    auto validation = validate_resource_set_locked(request.orphan_resources, orphan_indices);
    if (!validation.ok()) {
      return Result<ResourceSlotStopFenceResultDTO>::Err(validation.error());
    }
    for (const auto index : orphan_indices) {
      const auto& slot = slots_[index];
      if (slot.state != ResourceSlotState::kUnreconciledExternalOrOrphanGoal ||
          slot.owner.has_value()) {
        return reject_value<ResourceSlotStopFenceResultDTO>(
            "batch Stop orphan scope is not an unreconciled unowned resource",
            "resource_stop_fence_orphan_state_invalid", ErrorCode::kInvalidStateTransition);
      }
      if (request.stop_generation <= slot.stop_generation) {
        return reject_value<ResourceSlotStopFenceResultDTO>(
            "batch Stop generation does not advance the orphan resource",
            "resource_stop_generation_stale");
      }
    }
  }

  ResourceSlotStopFenceResultDTO result;
  result.operation_resource_generation = operation_resource_generation_ + 1U;
  result.stop_generation = request.stop_generation;
  result.revised_owners.reserve(request.owners.size());
  for (const auto& row : request.owners) {
    auto revised = row.expected_owner;
    revised.operation_revision = row.next_operation_revision;
    result.revised_owners.push_back(std::move(revised));
  }
  result.orphan_resources = request.orphan_resources;

  // All validation and every potentially throwing allocation/copy is complete.
  // The remaining scalar writes cannot expose a partially accepted fence.
  for (std::size_t row_index = 0U; row_index < request.owners.size(); ++row_index) {
    for (const auto slot_index : owner_indices[row_index]) {
      auto& slot = slots_[slot_index];
      slot.state = ResourceSlotState::kStopping;
      slot.owner->operation_revision = request.owners[row_index].next_operation_revision;
      slot.stop_generation = request.stop_generation;
    }
  }
  for (const auto index : orphan_indices) {
    slots_[index].stop_generation = request.stop_generation;
  }
  advance_generation_locked();
  return Result<ResourceSlotStopFenceResultDTO>::Ok(std::move(result));
}

Result<void> ResourceSlotTable::release(const ResourceSlotOwnerDTO& owner,
                                        const std::vector<ResourceId>& resources) {
  std::lock_guard lock(*admission_mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  for (const auto index : indices) {
    const auto& slot = slots_[index];
    if (!slot.owner.has_value() || *slot.owner != owner) {
      return reject("resource release owner does not match the slot", "resource_owner_mismatch");
    }
    if (slot.state != ResourceSlotState::kReserved && slot.state != ResourceSlotState::kTerminal) {
      return reject("only a reservation or terminal slot can be released",
                    "resource_release_state_invalid", ErrorCode::kInvalidStateTransition);
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return generation;
  }
  for (const auto index : indices) {
    slots_[index].state = ResourceSlotState::kFree;
    slots_[index].owner.reset();
  }
  advance_generation_locked();
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::release_batch(
    const std::vector<ResourceSlotReservationOwnerDTO>& owners) {
  if (owners.empty() || owners.size() > slots_.size() ||
      !std::is_sorted(owners.begin(), owners.end(),
                      [](const auto& left, const auto& right) {
                        return reservation_owner_key(left) < reservation_owner_key(right);
                      }) ||
      std::adjacent_find(owners.begin(), owners.end(), [](const auto& left, const auto& right) {
        return reservation_owner_key(left) == reservation_owner_key(right);
      }) != owners.end()) {
    return reject("coordinated resource release owners are not canonical",
                  "resource_release_batch_not_canonical", ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(*admission_mutex_);
  std::vector<std::size_t> assignments;
  assignments.reserve(slots_.size());
  std::set<ResourceId> complete_resources;
  std::vector<std::size_t> indices;
  for (const auto& row : owners) {
    if (!row.owner.valid() || row.resources.empty() ||
        !std::is_sorted(row.resources.begin(), row.resources.end()) ||
        std::adjacent_find(row.resources.begin(), row.resources.end()) != row.resources.end()) {
      return reject("coordinated resource release row is invalid",
                    "resource_release_batch_row_invalid", ErrorCode::kInvalidRequest);
    }
    auto validation = validate_resource_set_locked(row.resources, indices);
    if (!validation.ok()) {
      return validation;
    }
    for (std::size_t offset = 0U; offset < row.resources.size(); ++offset) {
      if (!complete_resources.insert(row.resources[offset]).second) {
        return reject("coordinated resource release scopes overlap",
                      "resource_release_batch_scope_overlap", ErrorCode::kInvalidRequest);
      }
      const auto index = indices[offset];
      const auto& slot = slots_[index];
      if (!slot.owner.has_value() || *slot.owner != row.owner) {
        return reject("resource release owner does not match the slot", "resource_owner_mismatch");
      }
      if (slot.state != ResourceSlotState::kReserved &&
          slot.state != ResourceSlotState::kTerminal) {
        return reject("resource release requires reserved or terminal state",
                      "resource_release_state_invalid", ErrorCode::kInvalidStateTransition);
      }
      assignments.push_back(index);
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return generation;
  }
  for (const auto index : assignments) {
    slots_[index].state = ResourceSlotState::kFree;
    slots_[index].owner.reset();
  }
  advance_generation_locked();
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::mark_unreconciled_external_or_orphan_goal(
    const std::vector<ResourceId>& resources) {
  std::lock_guard lock(*admission_mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  for (const auto index : indices) {
    if (slots_[index].state != ResourceSlotState::kFree || slots_[index].owner.has_value()) {
      return reject("only free unowned resources can become orphan-unreconciled",
                    "resource_orphan_mark_state_invalid", ErrorCode::kInvalidStateTransition);
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return generation;
  }
  for (const auto index : indices) {
    slots_[index].state = ResourceSlotState::kUnreconciledExternalOrOrphanGoal;
  }
  advance_generation_locked();
  return Result<void>::Ok();
}

Result<std::uint64_t> ResourceSlotTable::reconcile_unreconciled_external_or_orphan_goal(
    const std::uint64_t expected_operation_resource_generation,
    const std::vector<ResourceId>& resources) {
  std::lock_guard lock(*admission_mutex_);
  if (expected_operation_resource_generation != operation_resource_generation_) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                 "orphan-resource reconciliation names a stale slot-table generation",
                 {{"reason", "resource_orphan_reconciliation_generation_stale"}}});
  }
  if (!std::is_sorted(resources.begin(), resources.end())) {
    return Result<std::uint64_t>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                 "orphan-resource reconciliation scope is not canonical",
                 {{"reason", "resource_orphan_reconciliation_scope_noncanonical"}}});
  }
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return Result<std::uint64_t>::Err(validation.error());
  }
  for (const auto index : indices) {
    if (slots_[index].state != ResourceSlotState::kUnreconciledExternalOrOrphanGoal ||
        slots_[index].owner.has_value()) {
      return Result<std::uint64_t>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kInvalidStateTransition)),
                   "orphan-resource reconciliation requires an exact unowned orphan row",
                   {{"reason", "resource_orphan_reconciliation_state_invalid"}}});
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return Result<std::uint64_t>::Err(generation.error());
  }
  for (const auto index : indices) {
    slots_[index].state = ResourceSlotState::kFree;
  }
  advance_generation_locked();
  return Result<std::uint64_t>::Ok(operation_resource_generation_);
}

Result<void> ResourceSlotTable::reconfigure_runtime_inventory(
    std::vector<ResourceId> resources,
    std::vector<DomainActiveControlConfigurationDTO> active_control_configurations) {
  if (resources.size() > kMaxKernelResourceSlots) {
    return reject("candidate Kernel resource inventory is oversized",
                  "runtime_resource_inventory_count_invalid", ErrorCode::kInvalidRequest);
  }
  std::sort(resources.begin(), resources.end());
  if (std::any_of(resources.begin(), resources.end(),
                  [](const ResourceId& id) { return id.empty(); }) ||
      std::adjacent_find(resources.begin(), resources.end()) != resources.end() ||
      !canonical_active_configurations(active_control_configurations)) {
    return reject("candidate Kernel runtime inventory is noncanonical",
                  "runtime_resource_inventory_noncanonical", ErrorCode::kInvalidRequest);
  }

  std::lock_guard lock(*admission_mutex_);
  for (const auto& slot : slots_) {
    if (!std::binary_search(resources.begin(), resources.end(), slot.resource_id) &&
        (slot.state != ResourceSlotState::kFree || slot.owner.has_value())) {
      return reject("candidate runtime removes an occupied resource",
                    "runtime_resource_inventory_removes_occupied_resource");
    }
    if (!slot.owner.has_value()) {
      continue;
    }
    const auto configuration = std::lower_bound(
        active_control_configurations.begin(), active_control_configurations.end(),
        slot.owner->control_domain_id,
        [](const auto& row, const ControlDomainId& id) { return row.control_domain_id < id; });
    if (configuration == active_control_configurations.end() ||
        configuration->control_domain_id != slot.owner->control_domain_id ||
        configuration->active_control_configuration_id !=
            slot.owner->active_control_configuration_id ||
        configuration->active_control_configuration_generation !=
            slot.owner->active_control_configuration_generation) {
      return reject("candidate runtime changes an occupied control configuration",
                    "runtime_active_control_configuration_changed");
    }
  }
  auto generation = require_generation_available_locked();
  if (!generation.ok()) {
    return generation;
  }

  std::vector<ResourceSlotSnapshotDTO> candidate_slots;
  candidate_slots.reserve(resources.size());
  for (auto& resource : resources) {
    const auto existing =
        std::lower_bound(slots_.begin(), slots_.end(), resource,
                         [](const ResourceSlotSnapshotDTO& slot, const ResourceId& id) {
                           return slot.resource_id < id;
                         });
    if (existing != slots_.end() && existing->resource_id == resource) {
      candidate_slots.push_back(*existing);
    } else {
      candidate_slots.push_back(ResourceSlotSnapshotDTO{
          std::move(resource), ResourceSlotState::kFree, std::nullopt, StopGeneration(0U)});
    }
  }
  slots_ = std::move(candidate_slots);
  active_control_configurations_ = std::move(active_control_configurations);
  advance_generation_locked();
  return Result<void>::Ok();
}

std::vector<ResourceSlotSnapshotDTO> ResourceSlotTable::snapshot() const {
  std::lock_guard lock(*admission_mutex_);
  return slots_;
}

ResourceSlotTableSnapshotDTO ResourceSlotTable::coherent_snapshot() const {
  std::lock_guard lock(*admission_mutex_);
  return ResourceSlotTableSnapshotDTO{operation_resource_generation_, slots_};
}

ResourceSlotAdmissionSnapshotDTO ResourceSlotTable::admission_snapshot(
    const std::vector<ResourceId>& affected_resources) const {
  std::lock_guard lock(*admission_mutex_);
  std::set<ResourceId> affected;
  for (const auto& resource : affected_resources) {
    if (resource.empty() || !affected.insert(resource).second) {
      throw std::invalid_argument("affected resource admission scope is noncanonical");
    }
  }
  std::set<OperationId> queued;
  std::set<OperationId> active;
  bool unreconciled_orphan_present = false;
  for (const auto& slot : slots_) {
    if (!affected.contains(slot.resource_id)) {
      continue;
    }
    if (slot.state == ResourceSlotState::kUnreconciledExternalOrOrphanGoal) {
      unreconciled_orphan_present = true;
      continue;
    }
    if (!slot.owner.has_value()) {
      continue;
    }
    if (slot.state == ResourceSlotState::kReserved) {
      queued.insert(slot.owner->operation_id);
    } else if (slot.state == ResourceSlotState::kDispatched ||
               slot.state == ResourceSlotState::kStopping ||
               slot.state == ResourceSlotState::kTerminal) {
      active.insert(slot.owner->operation_id);
    }
  }
  return ResourceSlotAdmissionSnapshotDTO{
      operation_resource_generation_, active_control_configurations_,
      static_cast<std::uint64_t>(queued.size()),
      static_cast<std::uint64_t>(active.size()) + (unreconciled_orphan_present ? 1U : 0U)};
}

std::vector<OperationId> ResourceSlotTable::active_operation_ids() const {
  std::lock_guard lock(*admission_mutex_);
  std::vector<OperationId> operations;
  operations.reserve(slots_.size());
  for (const auto& slot : slots_) {
    if (slot.owner.has_value()) {
      operations.push_back(slot.owner->operation_id);
    }
  }
  std::sort(operations.begin(), operations.end());
  operations.erase(std::unique(operations.begin(), operations.end()), operations.end());
  return operations;
}

bool ResourceSlotTable::shares_admission_mutex(
    const std::shared_ptr<std::recursive_mutex>& admission_mutex) const noexcept {
  return admission_mutex != nullptr && admission_mutex_ == admission_mutex;
}

}  // namespace manyforge::core
