#include <algorithm>
#include <limits>
#include <manyforge_core/resource_slot_table.hpp>
#include <set>
#include <stdexcept>
#include <string>
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
    slots_.push_back(
        ResourceSlotSnapshotDTO{std::move(resource), ResourceSlotState::kFree, std::nullopt});
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
  if (active_operation_id_.has_value()) {
    return reject("I6 admits only one active Kernel operation", "i6_single_operation_busy");
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
  active_operation_id_ = owner.operation_id;
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
  if (!active_operation_id_.has_value() || *active_operation_id_ != owner.operation_id) {
    return reject("resource transition names a stale operation", "resource_owner_stale");
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

void ResourceSlotTable::clear_active_operation_if_unused_locked() {
  if (!active_operation_id_.has_value()) {
    return;
  }
  const auto in_use =
      std::any_of(slots_.begin(), slots_.end(), [&](const ResourceSlotSnapshotDTO& slot) {
        return slot.owner.has_value() && slot.owner->operation_id == *active_operation_id_;
      });
  if (!in_use) {
    active_operation_id_.reset();
  }
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
  clear_active_operation_if_unused_locked();
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
      candidate_slots.push_back(
          ResourceSlotSnapshotDTO{std::move(resource), ResourceSlotState::kFree, std::nullopt});
    }
  }
  slots_ = std::move(candidate_slots);
  active_control_configurations_ = std::move(active_control_configurations);
  clear_active_operation_if_unused_locked();
  advance_generation_locked();
  return Result<void>::Ok();
}

std::vector<ResourceSlotSnapshotDTO> ResourceSlotTable::snapshot() const {
  std::lock_guard lock(*admission_mutex_);
  return slots_;
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

std::optional<OperationId> ResourceSlotTable::active_operation_id() const {
  std::lock_guard lock(*admission_mutex_);
  return active_operation_id_;
}

bool ResourceSlotTable::shares_admission_mutex(
    const std::shared_ptr<std::recursive_mutex>& admission_mutex) const noexcept {
  return admission_mutex != nullptr && admission_mutex_ == admission_mutex;
}

}  // namespace manyforge::core
