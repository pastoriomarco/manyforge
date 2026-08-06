#include <manyforge_core/resource_slot_table.hpp>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] Result<void> reject(std::string message, std::string reason,
                                  ErrorCode code =
                                      ErrorCode::kExecutionRejected) {
  return Result<void>::Err(ErrorDTO{std::string(to_string(code)),
                                    std::move(message),
                                    {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool legal_transition(ResourceSlotState current,
                                    ResourceSlotState next) noexcept {
  switch (current) {
    case ResourceSlotState::kReserved:
      return next == ResourceSlotState::kDispatched ||
             next == ResourceSlotState::kStopping ||
             next == ResourceSlotState::kTerminal;
    case ResourceSlotState::kDispatched:
      return next == ResourceSlotState::kStopping ||
             next == ResourceSlotState::kTerminal;
    case ResourceSlotState::kStopping:
      return next == ResourceSlotState::kTerminal;
    case ResourceSlotState::kFree:
    case ResourceSlotState::kTerminal:
    case ResourceSlotState::kFenced:
    case ResourceSlotState::kUnreconciledExternalOrOrphanGoal:
      return false;
  }
  return false;
}

}  // namespace

bool ResourceSlotOwnerDTO::valid() const noexcept {
  return !operation_id.empty() && operation_revision.nonzero() &&
         !participant_id.empty() && !control_domain_id.empty() &&
         !control_domain_revision.empty() && !control_binding_id.empty() &&
         !control_binding_revision.empty() && !controller_topology_id.empty() &&
         !topology_revision.empty() && !control_route_id.empty() &&
         !active_control_configuration_id.empty() &&
         active_control_configuration_generation.nonzero();
}

ResourceSlotTable::ResourceSlotTable(std::vector<ResourceId> resources) {
  if (resources.empty() || resources.size() > kMaxKernelResourceSlots) {
    throw std::invalid_argument(
        "Kernel resource inventory is empty or exceeds its fixed bound");
  }
  std::sort(resources.begin(), resources.end());
  if (std::any_of(resources.begin(), resources.end(),
                  [](const ResourceId& id) { return id.empty(); }) ||
      std::adjacent_find(resources.begin(), resources.end()) !=
          resources.end()) {
    throw std::invalid_argument(
        "Kernel resource inventory contains an empty or duplicate identity");
  }
  slots_.reserve(resources.size());
  for (auto& resource : resources) {
    slots_.push_back(ResourceSlotSnapshotDTO{std::move(resource),
                                             ResourceSlotState::kFree,
                                             std::nullopt});
  }
}

Result<void> ResourceSlotTable::validate_resource_set_locked(
    const std::vector<ResourceId>& resources,
    std::vector<std::size_t>& indices) const {
  if (resources.empty() || resources.size() > slots_.size()) {
    return reject("resource scope is empty or exceeds the inventory",
                  "resource_scope_count_invalid",
                  ErrorCode::kInvalidRequest);
  }
  std::set<ResourceId> unique;
  indices.clear();
  indices.reserve(resources.size());
  for (const auto& resource : resources) {
    if (resource.empty() || !unique.insert(resource).second) {
      return reject("resource scope contains an empty or duplicate identity",
                    "resource_scope_identity_invalid",
                    ErrorCode::kInvalidRequest);
    }
    const auto iterator = std::lower_bound(
        slots_.begin(), slots_.end(), resource,
        [](const ResourceSlotSnapshotDTO& slot, const ResourceId& id) {
          return slot.resource_id < id;
        });
    if (iterator == slots_.end() || iterator->resource_id != resource) {
      return reject("resource scope names an unknown resource",
                    "resource_scope_unknown", ErrorCode::kInvalidRequest);
    }
    indices.push_back(
        static_cast<std::size_t>(std::distance(slots_.begin(), iterator)));
  }
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::reserve(
    const ResourceSlotOwnerDTO& owner,
    const std::vector<ResourceId>& resources) {
  if (!owner.valid()) {
    return reject("resource reservation owner is invalid",
                  "resource_owner_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  if (active_operation_id_.has_value()) {
    return reject("I6 admits only one active Kernel operation",
                  "i6_single_operation_busy");
  }
  for (const auto index : indices) {
    if (slots_[index].state != ResourceSlotState::kFree) {
      return reject("one or more resources are not free",
                    "resource_scope_busy");
    }
  }
  for (const auto index : indices) {
    slots_[index].state = ResourceSlotState::kReserved;
    slots_[index].owner = owner;
  }
  active_operation_id_ = owner.operation_id;
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::transition(
    const ResourceSlotOwnerDTO& owner,
    const std::vector<ResourceId>& resources, ResourceSlotState next_state) {
  if (!owner.valid()) {
    return reject("resource transition owner is invalid",
                  "resource_owner_invalid", ErrorCode::kInvalidRequest);
  }
  std::lock_guard lock(mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  if (!active_operation_id_.has_value() ||
      *active_operation_id_ != owner.operation_id) {
    return reject("resource transition names a stale operation",
                  "resource_owner_stale");
  }
  for (const auto index : indices) {
    const auto& slot = slots_[index];
    if (!slot.owner.has_value() || *slot.owner != owner) {
      return reject("resource transition owner does not match the slot",
                    "resource_owner_mismatch");
    }
    if (!legal_transition(slot.state, next_state)) {
      return reject("resource transition is illegal",
                    "resource_transition_illegal",
                    ErrorCode::kInvalidStateTransition);
    }
  }
  for (const auto index : indices) {
    slots_[index].state = next_state;
  }
  return Result<void>::Ok();
}

Result<ResourceSlotOwnerDTO> ResourceSlotTable::revise_owner(
    const ResourceSlotOwnerDTO& owner,
    const std::vector<ResourceId>& resources,
    OperationRevision next_operation_revision) {
  if (!owner.valid() || !next_operation_revision.nonzero() ||
      next_operation_revision <= owner.operation_revision) {
    return Result<ResourceSlotOwnerDTO>::Err(ErrorDTO{
        std::string(to_string(ErrorCode::kInvalidRequest)),
        "resource owner revision must advance monotonically",
        {{"reason", "resource_owner_revision_invalid"}}});
  }
  std::lock_guard lock(mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return Result<ResourceSlotOwnerDTO>::Err(validation.error());
  }
  for (const auto index : indices) {
    const auto& slot = slots_[index];
    if (!slot.owner.has_value() || *slot.owner != owner) {
      return Result<ResourceSlotOwnerDTO>::Err(ErrorDTO{
          std::string(to_string(ErrorCode::kExecutionRejected)),
          "resource owner revision names a stale or mismatched slot",
          {{"reason", "resource_owner_mismatch"}}});
    }
  }
  auto revised = owner;
  revised.operation_revision = next_operation_revision;
  for (const auto index : indices) {
    slots_[index].owner = revised;
  }
  return Result<ResourceSlotOwnerDTO>::Ok(std::move(revised));
}

void ResourceSlotTable::clear_active_operation_if_unused_locked() {
  if (!active_operation_id_.has_value()) {
    return;
  }
  const auto in_use = std::any_of(
      slots_.begin(), slots_.end(), [&](const ResourceSlotSnapshotDTO& slot) {
        return slot.owner.has_value() &&
               slot.owner->operation_id == *active_operation_id_;
      });
  if (!in_use) {
    active_operation_id_.reset();
  }
}

Result<void> ResourceSlotTable::release(
    const ResourceSlotOwnerDTO& owner,
    const std::vector<ResourceId>& resources) {
  std::lock_guard lock(mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  for (const auto index : indices) {
    const auto& slot = slots_[index];
    if (!slot.owner.has_value() || *slot.owner != owner) {
      return reject("resource release owner does not match the slot",
                    "resource_owner_mismatch");
    }
    if (slot.state != ResourceSlotState::kReserved &&
        slot.state != ResourceSlotState::kTerminal) {
      return reject("only a reservation or terminal slot can be released",
                    "resource_release_state_invalid",
                    ErrorCode::kInvalidStateTransition);
    }
  }
  for (const auto index : indices) {
    slots_[index].state = ResourceSlotState::kFree;
    slots_[index].owner.reset();
  }
  clear_active_operation_if_unused_locked();
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::fence(
    const std::vector<ResourceId>& resources) {
  std::lock_guard lock(mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  for (const auto index : indices) {
    if (slots_[index].state != ResourceSlotState::kFree) {
      return reject("only free resource slots can be fenced",
                    "resource_fence_state_invalid");
    }
  }
  for (const auto index : indices) {
    slots_[index].state = ResourceSlotState::kFenced;
  }
  return Result<void>::Ok();
}

Result<void> ResourceSlotTable::unfence(
    const std::vector<ResourceId>& resources) {
  std::lock_guard lock(mutex_);
  std::vector<std::size_t> indices;
  auto validation = validate_resource_set_locked(resources, indices);
  if (!validation.ok()) {
    return validation;
  }
  for (const auto index : indices) {
    if (slots_[index].state != ResourceSlotState::kFenced ||
        slots_[index].owner.has_value()) {
      return reject("only unowned fenced resource slots can be unfenced",
                    "resource_unfence_state_invalid");
    }
  }
  for (const auto index : indices) {
    slots_[index].state = ResourceSlotState::kFree;
  }
  return Result<void>::Ok();
}

std::vector<ResourceSlotSnapshotDTO> ResourceSlotTable::snapshot() const {
  std::lock_guard lock(mutex_);
  return slots_;
}

std::optional<OperationId> ResourceSlotTable::active_operation_id() const {
  std::lock_guard lock(mutex_);
  return active_operation_id_;
}

}  // namespace manyforge::core
