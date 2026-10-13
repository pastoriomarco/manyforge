#pragma once

#include <chrono>
#include <functional>
#include <manyforge_core/cclc_domain_reservation_table.hpp>
#include <manyforge_core/configured_robot_measured_state.hpp>
#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/icclc_brand_lifecycle_port_resolver.hpp>
#include <manyforge_core/ports/icclc_component_command_readiness_port.hpp>
#include <manyforge_core/ports/icclc_controller_lifecycle_port_resolver.hpp>
#include <manyforge_core/ports/icclc_current_component_snapshot_source.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/ports/icclc_kernel_route_configuration_fence_port.hpp>
#include <manyforge_core/ports/icclc_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/ports/icclc_route_configuration_publication_port.hpp>
#include <manyforge_core/ports/icclc_route_selection_current_snapshot_source.hpp>
#include <manyforge_core/ports/icclc_route_selection_template_source.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/component_lifecycle_readiness.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::deployment {
class CCLCRuntimeComposition;
}

namespace manyforge::core {

struct ComponentLifecycleCoordinatorConfig final {
  /// Startup-unique seed. The journal mints the advertised request namespace.
  RequestOwnerEpoch request_owner_epoch;
  std::size_t request_membership_capacity;
  std::size_t lifecycle_worker_count;
  std::int64_t observation_poll_period_ns;
  std::int64_t route_selection_timeout_duration_ns{30'000'000'000LL};
  /// Exact settled predecessor required only for the initial journal owner transition.
  std::optional<RequestOwnerEpoch> quiescent_predecessor{};

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

/// Exact immutable publication and configured membership; neither implies
/// that a configured component has a live handle or is execution-ready.
struct ComponentLifecycleProductIdentity final {
  ActiveBundlePublicationReceiptDTO publication;
  std::vector<ControllableComponentId> configured_components;
};

/// One product dependency set. An owning composition aliases this value with
/// its complete product lifetime; fixed-reference callers retain their
/// existing obligation to keep supplied dependencies alive through shutdown.
struct ComponentLifecycleProductContext final {
  const ICCLCCurrentComponentSnapshotSource& current_snapshot_source;
  const ICCLCLifecyclePlanCatalogSource& lifecycle_plan_catalog_source;
  const ICCLCBrandLifecyclePortResolver& lifecycle_port_resolver;
  const std::optional<ComponentLifecycleCoordinatorRouteServices> route_services;
  ICCLCComponentCommandReadinessPort* const component_command_readiness_port;
  /// Absent only for existing fixed-reference callers; absence never grants
  /// exchange permission or proves that a component does not exist.
  const std::optional<ComponentLifecycleProductIdentity> identity{};
  const ICCLCRouteSelectionTemplateSource* const route_template_source{};
  /// Owned complete configured robot membership and exact measured readers.
  /// Unwired legacy contexts may omit this; absence is never measured proof.
  const std::shared_ptr<const ConfiguredRobotMeasuredStateInventory> measured_state_inventory{};
  /// Pure capture from this installed product's existing route owner; current
  /// association is checked both before and after health/measured reads.
  const std::function<Result<ComponentLifecycleReadinessSourceView>(
      const ComponentLifecycleCurrentSnapshotDTO&)> capture_readiness_sources{};
};

/// One accounted product operation. The shared lease safely encloses the
/// accounting even if released after shutdown. Product use still requires the
/// original owner and its borrowed dependencies to be alive. Destruction must
/// occur outside the owner's admission exclusion.
class ComponentLifecycleProductPin final {
 public:
  ComponentLifecycleProductPin(ComponentLifecycleProductPin&&) noexcept = default;
  ComponentLifecycleProductPin& operator=(ComponentLifecycleProductPin&&) noexcept = default;
  ComponentLifecycleProductPin(const ComponentLifecycleProductPin&) = delete;
  ComponentLifecycleProductPin& operator=(const ComponentLifecycleProductPin&) = delete;
  [[nodiscard]] const ComponentLifecycleProductContext& context() const noexcept {
    return *context_;
  }

 private:
  friend class ComponentLifecycleCoordinator;
  friend class manyforge::deployment::CCLCRuntimeComposition;
  ComponentLifecycleProductPin(std::shared_ptr<const ComponentLifecycleProductContext> context,
                               std::shared_ptr<void> lease) noexcept
      : context_(std::move(context)), lease_(std::move(lease)) {}
  std::shared_ptr<const ComponentLifecycleProductContext> context_;
  std::shared_ptr<void> lease_;
};

enum class ComponentLifecycleProductExchangePhase { kPending, kCompleted };

/// Observation of an original owner-local exchange, never an authority DTO.
/// Blocking and foreign inputs return errors rather than perpetual pending.
struct ComponentLifecycleProductExchangeObservation final {
  ComponentLifecycleProductExchangePhase phase;
  RequestOwnerEpoch owner_epoch;
  ActiveBundlePublicationReceiptDTO previous_publication;
  ActiveBundlePublicationReceiptDTO current_publication;
  std::string reason;
};

/// Original host capability, retained only in the live accepted runtime. The
/// callback positively pins the original Apply/CCLC authority and actual
/// installation through the supplied synchronous action. Journal evidence
/// cannot recreate it. The reader proves the current public pointer head.
struct ComponentLifecycleCoordinatorApplyBringUpServices final {
  std::function<Result<void>(const ComponentLifecycleStatusDTO&,
                             const std::function<Result<void>()>&)>
      with_original_live_parent;
  const IActiveBundlePublicationReader* active_publication_reader{};
  ActiveBundlePublicationRequestDTO expected_publication;
  ActiveBundlePublicationReceiptDTO expected_previous_publication;
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
      std::shared_ptr<const ComponentLifecycleProductContext> product_context,
      IKernelCellAdmissionPort& kernel_cell_admission_port, IClock& clock, IScheduler& scheduler);

  ComponentLifecycleCoordinator(
      ComponentLifecycleCoordinatorConfig config, ICCLCJournal& journal,
      CCLCDomainReservationTable& domain_reservations,
      const ICCLCCurrentComponentSnapshotSource& current_snapshot_source,
      const ICCLCLifecyclePlanCatalogSource& lifecycle_plan_catalog_source,
      const ICCLCBrandLifecyclePortResolver& lifecycle_port_resolver,
      IKernelCellAdmissionPort& kernel_cell_admission_port, IClock& clock,
      IScheduler& scheduler,
      std::optional<ComponentLifecycleCoordinatorRouteServices> route_services = std::nullopt,
      ICCLCComponentCommandReadinessPort* component_command_readiness_port = nullptr);
  ~ComponentLifecycleCoordinator();

  ComponentLifecycleCoordinator(const ComponentLifecycleCoordinator&) = delete;
  ComponentLifecycleCoordinator& operator=(const ComponentLifecycleCoordinator&) = delete;

  /// Starts worker capacity before rotating the durable owner epoch. New
  /// admission opens only after the returned recovery set is empty or fully
  /// reconciled.
  [[nodiscard]] Result<void> Start();

  /// One product version through the complete caller operation. Journal-only
  /// request/status reads do not acquire this pin and remain available.
  [[nodiscard]] Result<ComponentLifecycleProductPin> AcquireProductPin() const;

  /// One side-effect-free component projection under one accounted Q105 pin.
  /// Target callbacks read only the existing driver/journal owners. Missing
  /// sources stay unavailable; this never invokes Observe or obtains a claim.
  [[nodiscard]] Result<CCLCComponentReadDTO> ReadCurrentComponentView(
      const ControllableComponentId& component,
      const std::function<Result<ComponentLifecycleTargetViewDTO>()>& read_target,
      const std::function<Result<ComponentLifecycleReadinessTargetFactsDTO>()>& read_target_facts) const;

  /// Original composition-local publication handoff. Prepared dependencies
  /// carry lifetime/identity only; this method additionally proves the actual
  /// owner's admissions, transactions, worker/callback tails and journal are
  /// quiescent. No owner epoch, follower or transport is recreated.
  [[nodiscard]] Result<ComponentLifecycleProductExchangeObservation> TryExchangeProductContext(
      const std::shared_ptr<const ComponentLifecycleProductContext>& expected_previous,
      const std::shared_ptr<const ComponentLifecycleProductContext>& prepared_successor,
      const RequestOwnerEpoch& expected_owner,
      const ActiveBundlePublicationReceiptDTO& expected_previous_publication);

  /// Classifies exact replay before current-state/capacity checks, then admits
  /// accepted-new work through exact scope reservation and durable CAS.
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent);
  /// Original owner-local whole-pass submission. The pin must belong to this
  /// owner/current context and enclose this call; pending exchange does not
  /// split an already admitted pass.
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent, const ComponentLifecycleProductPin& product_pin);

  /// Owner-private follower acceptance fenced by its actual target row.
  /// Exact request replay is classified before current-target validation.
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitTargetLifecycle(
      const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleTargetExpectationDTO& expected_target);
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitTargetLifecycle(
      const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleTargetExpectationDTO& expected_target,
      const ComponentLifecycleProductPin& product_pin);

  /// Owner-private non-real-time settlement read for retired target evidence.
  /// It never drains/cancels work or reopens admission; unavailable stays unknown.
  [[nodiscard]] Result<void> ReadLifecycleOwnerSettlement() const;
  [[nodiscard]] Result<CCLCLifecycleTargetRetirementOfferDTO> ReadLifecycleTargetRetirementOffer() const;
  [[nodiscard]] Result<CCLCLifecycleTargetRetirementResultDTO> RetireLifecycleTargetsForClose(
      const CCLCLifecycleTargetRetirementIntentDTO& intent);
  [[nodiscard]] Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>> ReadLifecycleTargetRetirementResult(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) const;
  [[nodiscard]] Result<void> ReadLifecycleOwnerSettlement(
      const CCLCLifecycleTargetRetirementScopeDTO& scope) const;


  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent);

  /// Read the original private request across owner replacement. The journal
  /// classifies retained/conflicting/expired results; absence grants no effect.
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadSelectedComponentBringUpRequestResult(const ComponentLifecycleIntentDTO& intent,
                                            const ContentDigest& stable_digest);
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadApplyComponentBringUpRequestResult(const ComponentLifecycleIntentDTO& intent,
                                         const ContentDigest& stable_digest);

  /// Owner-local accepted-new delegation. Exact replay precedes live checks;
  /// acceptance reserves the union but schedules no provider work.
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> AcceptApplyComponentBringUp(
      const ComponentLifecycleIntentDTO& intent, const ApplyComponentBringUpEvidenceDTO& evidence,
      const std::optional<ComponentLifecycleCurrentSnapshotDTO>& genuine_predecessor,
      const std::vector<BrandLifecyclePlanCatalogDTO>& candidate_catalogs,
      const std::vector<ResolvedLifecycleControlDomainScopeDTO>& qualified_candidate_domains,
      const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& candidate_provider_occurrences,
      ComponentLifecycleCoordinatorApplyBringUpServices services);

  /// Single-shot original runtime continuation after real installation. The
  /// live callback must pin the candidate registrations and actual completed
  /// retirement of exactly the old-only domains through all provider work.
  /// It must also pin the actual installed readiness port's lifetime. When
  /// omitted, the constructor port must belong to that installed product;
  /// absence of both ports refuses Continue.
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> ContinueApplyComponentBringUp(
      const ComponentLifecycleIntentDTO& intent, const ContentDigest& stable_digest,
      const std::vector<ResolvedLifecycleControlDomainScopeDTO>& installed_candidate_domains,
      const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& installed_candidate_providers,
      const std::vector<BrandLifecyclePortRegistrationDTO>& installed_candidate_ports,
      const std::vector<ControlDomainId>& retired_known_no_effect_domains,
      ICCLCComponentCommandReadinessPort* installed_candidate_readiness_port = nullptr);

  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& lifecycle_transaction_id);

  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO& intent);
  /// Owner-local read: qualify the current source and report observed controller
  /// truth, including mismatch. Requires no request admission, claim or fence;
  /// never publishes readiness, changes controllers or clears a fault. It uses
  /// the observation-only snapshot (no component lifecycle expectation), so a
  /// Declared component is observable while route selection stays refused.
  /// The caller runs this bounded adapter read off the scheduler/control lane.
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
  /// Exact sticky closure flag; observe before deliberately closing admission.
  [[nodiscard]] bool admission_fault_closed() const noexcept;
  [[nodiscard]] RequestOwnerEpoch request_owner_epoch() const;
  void CloseAdmission() noexcept;

  /// Operational bounded drain of admission, accepted work and callback tails.
  /// It does not advance semantic time or close ingress. Lifetime callers must
  /// first exclude new direct requests and stop their transport/follower ingress.
  [[nodiscard]] bool DrainFor(std::chrono::nanoseconds timeout);

 private:
  friend class manyforge::deployment::CCLCRuntimeComposition;
  [[nodiscard]] bool WithdrawProductExchange(
      const std::shared_ptr<const ComponentLifecycleProductContext>& previous,
      const std::shared_ptr<const ComponentLifecycleProductContext>& successor,
      const RequestOwnerEpoch& captured_owner);
  void CloseProductExchangeAdmission() noexcept;
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::core
