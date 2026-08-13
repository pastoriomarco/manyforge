#pragma once

#include <cstddef>
#include <manyforge_core/types/cclc.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxCCLCActiveDomainReservations = 64U;
inline constexpr std::size_t kMaxCCLCDomainReservationOwnerComponents = 4096U;

struct ComponentLifecycleDomainReservationOwnerDTO final {
  ControllableComponentId controllable_component_id;
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleDomainReservationOwnerDTO&,
                         const ComponentLifecycleDomainReservationOwnerDTO&) = default;
};

struct RouteSelectionDomainReservationOwnerDTO final {
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  std::vector<ControllableComponentId> controllable_component_ids;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteSelectionDomainReservationOwnerDTO&,
                         const RouteSelectionDomainReservationOwnerDTO&) = default;
};

using CCLCDomainReservationOwnerValueDTO = std::variant<ComponentLifecycleDomainReservationOwnerDTO,
                                                        RouteSelectionDomainReservationOwnerDTO>;

/// Closed tagged transaction-family owner for the one CCLC domain authority.
struct CCLCDomainReservationOwnerDTO final {
  CCLCRequestKindDTO request_kind;
  CCLCDomainReservationOwnerValueDTO owner;

  explicit CCLCDomainReservationOwnerDTO(
      ComponentLifecycleDomainReservationOwnerDTO lifecycle_owner)
      : request_kind(CCLCRequestKindDTO::kComponentLifecycle), owner(std::move(lifecycle_owner)) {}

  explicit CCLCDomainReservationOwnerDTO(RouteSelectionDomainReservationOwnerDTO route_owner)
      : request_kind(CCLCRequestKindDTO::kRouteSelection), owner(std::move(route_owner)) {}

  /// Lifecycle-only construction retained for the current L0.2 coordinator.
  /// A route tag through this overload is structurally invalid; route owners
  /// must carry their complete canonical component set.
  CCLCDomainReservationOwnerDTO(
      CCLCRequestKindDTO kind, ControllableComponentId controllable_component_id,
      std::optional<LifecycleTransactionId> lifecycle_transaction_id,
      std::optional<LifecycleTransactionGeneration> lifecycle_transaction_generation,
      std::optional<RouteSelectionTransactionId> route_selection_transaction_id,
      std::optional<RouteSelectionTransactionGeneration> route_selection_transaction_generation);

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCDomainReservationOwnerDTO&,
                         const CCLCDomainReservationOwnerDTO&) = default;
};

struct CCLCDomainReservationSnapshotDTO final {
  ControlDomainId control_domain_id;
  CCLCDomainReservationOwnerDTO owner;

  friend bool operator==(const CCLCDomainReservationSnapshotDTO&,
                         const CCLCDomainReservationSnapshotDTO&) = default;
};

/// Process-local deterministic domain reservation owned by CCLC.
///
/// The durable journal remains the transaction/replay authority. This table
/// is the single overlap authority for lifecycle and route-selection work. A
/// replacement CCLC restores every nonterminal journal transaction before
/// opening admission. Methods are bounded, atomic over the complete supplied
/// scope, perform no I/O, and never wait on a provider or Kernel call.
class CCLCDomainReservationTable final {
 public:
  CCLCDomainReservationTable() = default;

  CCLCDomainReservationTable(const CCLCDomainReservationTable&) = delete;
  CCLCDomainReservationTable& operator=(const CCLCDomainReservationTable&) = delete;

  /// Atomically reserve one canonical complete domain scope. Repeating the
  /// exact owner/scope is idempotent, which is the recovery path after owner
  /// replacement. Reusing either transaction ID or generation within its
  /// tagged family with a different owner/scope fails closed.
  [[nodiscard]] Result<void> reserve(const CCLCDomainReservationOwnerDTO& owner,
                                     const std::vector<ControlDomainId>& control_domain_ids);

  /// Atomically release only the exact complete owner/scope.
  [[nodiscard]] Result<void> release(const CCLCDomainReservationOwnerDTO& owner,
                                     const std::vector<ControlDomainId>& control_domain_ids);

  [[nodiscard]] std::vector<CCLCDomainReservationSnapshotDTO> snapshot() const;

 private:
  mutable std::mutex mutex_;
  std::vector<CCLCDomainReservationSnapshotDTO> reservations_;
};

}  // namespace manyforge::core
