#pragma once

#include <chrono>
#include <manyforge_core/cclc_domain_reservation_table.hpp>
#include <manyforge_core/ports/icclc_brand_lifecycle_port_resolver.hpp>
#include <manyforge_core/ports/icclc_component_command_readiness_port.hpp>
#include <manyforge_core/ports/icclc_controller_lifecycle_port_resolver.hpp>
#include <manyforge_core/ports/icclc_current_component_snapshot_source.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/ports/icclc_kernel_disable_handoff_port.hpp>
#include <manyforge_core/ports/icclc_kernel_route_configuration_fence_port.hpp>
#include <manyforge_core/ports/icclc_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/ports/icclc_route_configuration_publication_port.hpp>
#include <manyforge_core/ports/icclc_route_selection_current_snapshot_source.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <memory>
#include <optional>
#include <utility>

namespace manyforge::core {

struct ComponentLifecycleCoordinatorConfig final {
  /// Startup-unique seed. The journal mints the advertised request namespace.
  RequestOwnerEpoch request_owner_epoch;
  std::size_t request_membership_capacity;
  std::size_t lifecycle_worker_count;
  std::size_t kernel_worker_count;
  std::int64_t observation_poll_period_ns;
  std::int64_t route_selection_timeout_duration_ns{30'000'000'000LL};

  [[nodiscard]] bool locally_valid() const noexcept;
};

/// Optional complete L0.4 route-owner dependency set. All pointers are
/// required together. A lifecycle-only composition may omit the set only when
/// the durable journal has no interrupted route transaction.
struct ComponentLifecycleCoordinatorRouteServices final {
  const ICCLCRouteSelectionCurrentSnapshotSource* current_snapshot_source{};
  const ICCLCControllerLifecyclePortResolver* controller_lifecycle_port_resolver{};
  ICCLCKernelRouteConfigurationFencePort* kernel_route_fence_port{};
  ICCLCRouteConfigurationPublicationPort* configuration_publication_port{};

  [[nodiscard]] bool locally_valid() const noexcept;
};

/// Generic Controlled Component Lifecycle Coordinator transaction owner.
///
/// Open request-port calls perform bounded replay/admission work only. Brand and Kernel
/// calls run on fixed worker threads, never on an IScheduler callback. The
/// scheduler is used only to wake deferred observation work.
class ComponentLifecycleCoordinator final {
 public:
  ComponentLifecycleCoordinator(
      ComponentLifecycleCoordinatorConfig config, ICCLCJournal& journal,
      CCLCDomainReservationTable& domain_reservations,
      const ICCLCCurrentComponentSnapshotSource& current_snapshot_source,
      const ICCLCLifecyclePlanCatalogSource& lifecycle_plan_catalog_source,
      const ICCLCBrandLifecyclePortResolver& lifecycle_port_resolver,
      ICCLCKernelDisableHandoffPort& kernel_disable_handoff_port, IClock& clock,
      IScheduler& scheduler,
      IKernelCellAdmissionPort* kernel_cell_admission_port = nullptr,
      std::optional<ComponentLifecycleCoordinatorRouteServices> route_services = std::nullopt,
      ICCLCComponentCommandReadinessPort* component_command_readiness_port = nullptr);
  ~ComponentLifecycleCoordinator();

  ComponentLifecycleCoordinator(const ComponentLifecycleCoordinator&) = delete;
  ComponentLifecycleCoordinator& operator=(const ComponentLifecycleCoordinator&) = delete;

  /// Starts worker capacity before rotating the durable owner epoch. New
  /// admission opens only after the returned recovery set is empty or fully
  /// reconciled.
  [[nodiscard]] Result<void> Start();

  /// Classifies exact replay before current-state/capacity checks, then admits
  /// accepted-new work through exact scope reservation and durable CAS.
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent);

  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent);

  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id);

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO& intent);
  /// Owner-local read: qualify the current source and report observed controller
  /// truth, including mismatch. Requires no request admission, claim or fence;
  /// never publishes readiness, changes controllers or clears a fault. The
  /// caller runs this bounded adapter read off the scheduler/control lane.
  [[nodiscard]] Result<
      std::pair<ControllerLifecycleObservationDTO, ControllerLifecycleReconciliationDTO>>
  ObserveRouteConfiguration(
      const RetainedBundleId& retained_bundle_id, const GraphRevision& graph_revision,
      const DesiredControlConfigurationDTO& desired_configuration,
      std::optional<ExpectedActiveControlConfigurationDTO> expected_current = {});
  /// Owner-local replacement recovery; not exposed by the open request port.
  /// Reconciles observed configuration only; its durable recovery claim never
  /// authorizes controller transitions, including when the observed route differs.
  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteRecovery(
      const RouteSelectionIntentDTO& intent, const ActiveControlConfigurationDTO& predecessor);

  [[nodiscard]] Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  ReadRouteSelectionRequestResult(const RouteSelectionIntentDTO& intent);

  [[nodiscard]] Result<std::optional<RouteSelectionStatusDTO>> LoadRouteSelectionStatus(
      const RouteSelectionTransactionId& route_selection_transaction_id);

  [[nodiscard]] bool accepting_new_requests() const noexcept;
  [[nodiscard]] RequestOwnerEpoch request_owner_epoch() const;
  void CloseAdmission() noexcept;

  /// Operational bounded drain. It does not advance semantic time.
  [[nodiscard]] bool DrainFor(std::chrono::nanoseconds timeout);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::core
