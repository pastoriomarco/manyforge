#pragma once

#include <manyforge_core/cclc_lifecycle_target_driver.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/ports/icclc_current_component_snapshot_source.hpp>
#include <manyforge_core/ports/icclc_route_selection_template_source.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kCCLCOpenRequestApiVersion = "manyforge.cclc_open_request.v1";
inline constexpr std::string_view kCCLCOpenResponseApiVersion = "manyforge.cclc_open_response.v1";
inline constexpr std::size_t kMaximumCCLCOpenRequestBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumCCLCOpenResponseBytes = 8U * 1024U * 1024U;

/// Narrow caller-neutral semantic surface shared by HTTP, ROS, CLI, Behavior,
/// and other open clients. Implementations own no policy or runtime truth;
/// every method delegates to the sole CCLC owner or its verified read source.
class ICCLCOpenRequestPort {
 public:
  virtual ~ICCLCOpenRequestPort() = default;

  [[nodiscard]] virtual RequestOwnerEpoch ReadRequestOwnerEpoch() const = 0;

  [[nodiscard]] virtual Result<CCLCLifecycleTargetRetirementOfferDTO>
  ReadLifecycleTargetRetirementOffer() {
    return Result<CCLCLifecycleTargetRetirementOfferDTO>::Err(
        {"cclc_target_retirement_unavailable", "target retirement is not composed", {}});
  }
  [[nodiscard]] virtual Result<CCLCLifecycleTargetRetirementResultDTO>
  RetireLifecycleTargetsForClose(const CCLCLifecycleTargetRetirementIntentDTO&) {
    return Result<CCLCLifecycleTargetRetirementResultDTO>::Err(
        {"cclc_target_retirement_unavailable", "target retirement is not composed", {}});
  }
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>>
  ReadLifecycleTargetRetirementResult(const CCLCLifecycleTargetRetirementIntentDTO&) {
    return Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>>::Err(
        {"cclc_target_retirement_unavailable", "target retirement is not composed", {}});
  }
  [[nodiscard]] virtual Result<void> ReadLifecycleOwnerSettlement(
      const CCLCLifecycleTargetRetirementScopeDTO&) {
    return Result<void>::Err(
        {"cclc_owner_settlement_unavailable", "owner settlement is not composed", {}});
  }
  [[nodiscard]] virtual Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) = 0;
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>>
  ReadLifecycleRequestResult(const ComponentLifecycleIntentDTO&) {
    return Result<std::optional<CCLCLifecycleRequestResultDTO>>::Err(
        {"cclc_result_lookup_unavailable", "lifecycle result lookup is not composed", {}});
  }
  [[nodiscard]] virtual Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& transaction_id) = 0;
  [[nodiscard]] virtual Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId& component_id) const = 0;
  /// Closed status projection from the actual composed owner. A legacy
  /// source override cannot manufacture readiness from a component snapshot.
  [[nodiscard]] virtual Result<CCLCComponentReadDTO> ReadCurrentComponentView(
      const ControllableComponentId&) const {
    return Result<CCLCComponentReadDTO>::Err(
        {"cclc_product_read_unavailable", "the grouped component owner read is not composed", {}});
  }
  /// Requested lifecycle target and progress shown beside the component
  /// snapshot. Both are absent when no target is recorded or composed.
  [[nodiscard]] virtual Result<ComponentLifecycleTargetViewDTO> ReadLifecycleTargetView(
      const ControllableComponentId&) const {
    return Result<ComponentLifecycleTargetViewDTO>::Ok({});
  }
  /// Durable component-incident history events after `after_sequence`, for
  /// the Headless historian pipe (contract item 5). None when not composed.
  [[nodiscard]] virtual Result<std::vector<ComponentIncidentEventDTO>> ReadIncidentEvents(
      std::uint64_t, std::size_t) {
    return Result<std::vector<ComponentIncidentEventDTO>>::Ok({});
  }
  [[nodiscard]] virtual Result<CCLCRouteSelectionTemplateSetDTO> ReadRouteSelectionTemplates(
      const ControllableComponentId& component_id) const = 0;
  [[nodiscard]] virtual Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO& intent) = 0;
  [[nodiscard]] virtual Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  ReadRouteSelectionRequestResult(const RouteSelectionIntentDTO&) {
    return Result<std::optional<CCLCRouteSelectionRequestResultDTO>>::Err(
        {"cclc_result_lookup_unavailable", "route result lookup is not composed", {}});
  }
  [[nodiscard]] virtual Result<std::optional<RouteSelectionStatusDTO>> LoadRouteSelectionStatus(
      const RouteSelectionTransactionId& transaction_id) = 0;
};

/// Zero-policy adapter over one coordinator plus its independently verified
/// current-component source. Both referents must outlive this adapter.
class ComponentLifecycleCoordinatorOpenRequestPort final : public ICCLCOpenRequestPort {
 public:
  ComponentLifecycleCoordinatorOpenRequestPort(
      ComponentLifecycleCoordinator& coordinator,
      const ICCLCCurrentComponentSnapshotSource& current_component_source,
      const ICCLCRouteSelectionTemplateSource& route_template_source,
      CCLCLifecycleTargetDriver* target_driver = nullptr) noexcept;
  /// Current composed product; each product read uses one owner pin. Journal,
  /// progress and retained request reads stay independent of pending exchange.
  explicit ComponentLifecycleCoordinatorOpenRequestPort(
      ComponentLifecycleCoordinator& coordinator,
      CCLCLifecycleTargetDriver* target_driver = nullptr) noexcept;

  [[nodiscard]] Result<CCLCLifecycleTargetRetirementOfferDTO> ReadLifecycleTargetRetirementOffer() override;
  [[nodiscard]] Result<CCLCLifecycleTargetRetirementResultDTO> RetireLifecycleTargetsForClose(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleTargetRetirementResultDTO>> ReadLifecycleTargetRetirementResult(
      const CCLCLifecycleTargetRetirementIntentDTO& intent) override;
  [[nodiscard]] Result<void> ReadLifecycleOwnerSettlement(
      const CCLCLifecycleTargetRetirementScopeDTO& scope) override;
  [[nodiscard]] RequestOwnerEpoch ReadRequestOwnerEpoch() const override;
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent) override;
  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& transaction_id) override;
  [[nodiscard]] Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId& component_id) const override;
  [[nodiscard]] Result<CCLCComponentReadDTO> ReadCurrentComponentView(
      const ControllableComponentId& component_id) const override;
  [[nodiscard]] Result<ComponentLifecycleTargetViewDTO> ReadLifecycleTargetView(
      const ControllableComponentId& component_id) const override;
  [[nodiscard]] Result<std::vector<ComponentIncidentEventDTO>> ReadIncidentEvents(
      std::uint64_t after_sequence, std::size_t limit) override;
  [[nodiscard]] Result<CCLCRouteSelectionTemplateSetDTO> ReadRouteSelectionTemplates(
      const ControllableComponentId& component_id) const override;
  [[nodiscard]] Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO& intent) override;
  [[nodiscard]] Result<std::optional<CCLCRouteSelectionRequestResultDTO>>
  ReadRouteSelectionRequestResult(const RouteSelectionIntentDTO& intent) override;
  [[nodiscard]] Result<std::optional<RouteSelectionStatusDTO>> LoadRouteSelectionStatus(
      const RouteSelectionTransactionId& transaction_id) override;

 private:
  ComponentLifecycleCoordinator& coordinator_;
  const ICCLCCurrentComponentSnapshotSource* current_component_source_{};
  const ICCLCRouteSelectionTemplateSource* route_template_source_{};
  CCLCLifecycleTargetDriver* target_driver_;
};

/// Decode one exact RFC-8785 request, stamp the supplied transport-derived
/// attribution (the payload carries none), invoke one owner-port method, and
/// return one canonical response. Invalid requests and semantic owner errors
/// are represented as `ok:false` responses; infrastructure/canonicalization
/// failure is returned as Result error. This function performs no I/O beyond
/// the supplied port call and retains no replay or current state.
[[nodiscard]] Result<std::string> ExecuteCanonicalCCLCOpenRequest(
    std::string_view canonical_request, ICCLCOpenRequestPort& port,
    RequestAttributionDTO transport_attribution);

}  // namespace manyforge::core
