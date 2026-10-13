#include <algorithm>
#include <iterator>
#include <manyforge_core/cclc_domain_reservation_table.hpp>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

template <typename Id>
[[nodiscard]] bool bounded_identity(const Id& identity) noexcept {
  return !identity.empty() && identity.value().size() <= kMaxLifecycleIdentityBytes;
}

[[nodiscard]] Result<void> reject(std::string code, std::string message, std::string reason) {
  return Result<void>::Err(
      ErrorDTO{std::move(code), std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool canonical_scope(
    const std::vector<ControlDomainId>& control_domain_ids) noexcept {
  return !control_domain_ids.empty() &&
         control_domain_ids.size() <= kMaxLifecycleDisableDomainUnion &&
         std::is_sorted(control_domain_ids.begin(), control_domain_ids.end()) &&
         std::adjacent_find(control_domain_ids.begin(), control_domain_ids.end()) ==
             control_domain_ids.end() &&
         std::all_of(control_domain_ids.begin(), control_domain_ids.end(),
                     [](const ControlDomainId& id) { return bounded_identity(id); });
}

[[nodiscard]] std::vector<ControlDomainId> scope_for(
    const std::vector<CCLCDomainReservationSnapshotDTO>& rows,
    const CCLCDomainReservationOwnerDTO& owner) {
  std::vector<ControlDomainId> result;
  for (const auto& row : rows) {
    if (row.owner == owner) {
      result.push_back(row.control_domain_id);
    }
  }
  return result;
}

[[nodiscard]] std::vector<CCLCDomainReservationOwnerDTO> active_owners(
    const std::vector<CCLCDomainReservationSnapshotDTO>& rows) {
  std::vector<CCLCDomainReservationOwnerDTO> result;
  for (const auto& row : rows) {
    if (std::find(result.begin(), result.end(), row.owner) == result.end()) {
      result.push_back(row.owner);
    }
  }
  return result;
}

[[nodiscard]] bool transaction_identity_conflicts(
    const CCLCDomainReservationOwnerDTO& left,
    const CCLCDomainReservationOwnerDTO& right) noexcept {
  if (left.request_kind != right.request_kind) {
    return false;
  }
  switch (left.request_kind) {
    case CCLCRequestKindDTO::kLifecycleTargetRetirement:
      return true; // Retirement has no domain effect reservation.
    case CCLCRequestKindDTO::kComponentLifecycle: {
      const auto* left_owner =
          std::get_if<ComponentLifecycleDomainReservationOwnerDTO>(&left.owner);
      const auto* right_owner =
          std::get_if<ComponentLifecycleDomainReservationOwnerDTO>(&right.owner);
      return left_owner == nullptr || right_owner == nullptr ||
             left_owner->lifecycle_transaction_id == right_owner->lifecycle_transaction_id ||
             left_owner->lifecycle_transaction_generation ==
                 right_owner->lifecycle_transaction_generation;
    }
    case CCLCRequestKindDTO::kRouteSelection: {
      const auto* left_owner = std::get_if<RouteSelectionDomainReservationOwnerDTO>(&left.owner);
      const auto* right_owner = std::get_if<RouteSelectionDomainReservationOwnerDTO>(&right.owner);
      return left_owner == nullptr || right_owner == nullptr ||
             left_owner->route_selection_transaction_id ==
                 right_owner->route_selection_transaction_id ||
             left_owner->route_selection_transaction_generation ==
                 right_owner->route_selection_transaction_generation;
    }
  }
  return true;
}

}  // namespace

bool ComponentLifecycleDomainReservationOwnerDTO::locally_valid() const noexcept {
  return bounded_identity(controllable_component_id) &&
         bounded_identity(lifecycle_transaction_id) && lifecycle_transaction_generation.nonzero();
}

bool RouteSelectionDomainReservationOwnerDTO::locally_valid() const noexcept {
  return bounded_identity(route_selection_transaction_id) &&
         route_selection_transaction_generation.nonzero() && !controllable_component_ids.empty() &&
         controllable_component_ids.size() <= kMaxCCLCDomainReservationOwnerComponents &&
         std::is_sorted(controllable_component_ids.begin(), controllable_component_ids.end()) &&
         std::adjacent_find(controllable_component_ids.begin(), controllable_component_ids.end()) ==
             controllable_component_ids.end() &&
         std::all_of(controllable_component_ids.begin(), controllable_component_ids.end(),
                     [](const auto& component_id) { return bounded_identity(component_id); });
}

CCLCDomainReservationOwnerDTO::CCLCDomainReservationOwnerDTO(
    CCLCRequestKindDTO kind, ControllableComponentId controllable_component_id,
    std::optional<LifecycleTransactionId> lifecycle_transaction_id,
    std::optional<LifecycleTransactionGeneration> lifecycle_transaction_generation,
    std::optional<RouteSelectionTransactionId> route_selection_transaction_id,
    std::optional<RouteSelectionTransactionGeneration> route_selection_transaction_generation)
    : request_kind(kind),
      owner(ComponentLifecycleDomainReservationOwnerDTO{
          std::move(controllable_component_id),
          lifecycle_transaction_id.value_or(LifecycleTransactionId("")),
          lifecycle_transaction_generation.value_or(LifecycleTransactionGeneration(0U))}) {
  if (request_kind != CCLCRequestKindDTO::kComponentLifecycle ||
      route_selection_transaction_id.has_value() ||
      route_selection_transaction_generation.has_value()) {
    owner = RouteSelectionDomainReservationOwnerDTO{
        route_selection_transaction_id.value_or(RouteSelectionTransactionId("")),
        route_selection_transaction_generation.value_or(RouteSelectionTransactionGeneration(0U)),
        {}};
  }
}

bool CCLCDomainReservationOwnerDTO::locally_valid() const noexcept {
  switch (request_kind) {
    case CCLCRequestKindDTO::kLifecycleTargetRetirement:
      return false; // Journal membership does not mint an effect owner.
    case CCLCRequestKindDTO::kComponentLifecycle:
      return std::holds_alternative<ComponentLifecycleDomainReservationOwnerDTO>(owner) &&
             std::get<ComponentLifecycleDomainReservationOwnerDTO>(owner).locally_valid();
    case CCLCRequestKindDTO::kRouteSelection:
      return std::holds_alternative<RouteSelectionDomainReservationOwnerDTO>(owner) &&
             std::get<RouteSelectionDomainReservationOwnerDTO>(owner).locally_valid();
  }
  return false;
}

Result<void> CCLCDomainReservationTable::reserve(
    const CCLCDomainReservationOwnerDTO& owner,
    const std::vector<ControlDomainId>& control_domain_ids) {
  if (!owner.locally_valid() || !canonical_scope(control_domain_ids)) {
    return reject("cclc_domain_reservation_invalid",
                  "CCLC domain reservation owner or scope is invalid", "owner_or_scope_invalid");
  }

  std::lock_guard lock(mutex_);
  const auto exact_existing = scope_for(reservations_, owner);
  if (!exact_existing.empty()) {
    if (exact_existing == control_domain_ids) {
      return Result<void>::Ok();
    }
    return reject("cclc_domain_reservation_conflict",
                  "CCLC transaction already owns a different scope", "transaction_scope_conflict");
  }

  const auto owners = active_owners(reservations_);
  for (const auto& active_owner : owners) {
    if (transaction_identity_conflicts(active_owner, owner)) {
      return reject("cclc_domain_reservation_conflict",
                    "CCLC transaction identity or generation was reused",
                    "transaction_identity_reused");
    }
  }
  if (owners.size() >= kMaxCCLCActiveDomainReservations) {
    return reject("request_queue_full", "CCLC active transaction capacity is exhausted",
                  "active_transaction_capacity_exhausted");
  }

  for (const auto& domain_id : control_domain_ids) {
    const auto position =
        std::lower_bound(reservations_.begin(), reservations_.end(), domain_id,
                         [](const CCLCDomainReservationSnapshotDTO& row,
                            const ControlDomainId& id) { return row.control_domain_id < id; });
    if (position != reservations_.end() && position->control_domain_id == domain_id) {
      return reject("cclc_domain_busy", "one or more CCLC control domains are reserved",
                    "control_domain_reserved");
    }
  }

  std::vector<CCLCDomainReservationSnapshotDTO> additions;
  additions.reserve(control_domain_ids.size());
  for (const auto& domain_id : control_domain_ids) {
    additions.push_back({domain_id, owner});
  }
  std::vector<CCLCDomainReservationSnapshotDTO> merged;
  merged.reserve(reservations_.size() + additions.size());
  std::merge(reservations_.begin(), reservations_.end(), additions.begin(), additions.end(),
             std::back_inserter(merged),
             [](const CCLCDomainReservationSnapshotDTO& left,
                const CCLCDomainReservationSnapshotDTO& right) {
               return left.control_domain_id < right.control_domain_id;
             });
  reservations_ = std::move(merged);
  return Result<void>::Ok();
}

Result<void> CCLCDomainReservationTable::release(
    const CCLCDomainReservationOwnerDTO& owner,
    const std::vector<ControlDomainId>& control_domain_ids) {
  if (!owner.locally_valid() || !canonical_scope(control_domain_ids)) {
    return reject("cclc_domain_reservation_invalid",
                  "CCLC domain release owner or scope is invalid", "owner_or_scope_invalid");
  }

  std::lock_guard lock(mutex_);
  const auto exact_existing = scope_for(reservations_, owner);
  if (exact_existing != control_domain_ids) {
    return reject(
        "cclc_domain_reservation_conflict",
        "CCLC domain release does not match the complete owner scope",
        exact_existing.empty() ? "transaction_not_reserved" : "transaction_scope_conflict");
  }
  reservations_.erase(std::remove_if(reservations_.begin(), reservations_.end(),
                                     [&](const CCLCDomainReservationSnapshotDTO& row) {
                                       return row.owner == owner;
                                     }),
                      reservations_.end());
  return Result<void>::Ok();
}

std::vector<CCLCDomainReservationSnapshotDTO> CCLCDomainReservationTable::snapshot() const {
  std::lock_guard lock(mutex_);
  return reservations_;
}

}  // namespace manyforge::core
