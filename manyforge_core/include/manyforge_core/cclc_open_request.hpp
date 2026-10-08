#pragma once

#include <manyforge_core/cclc_lifecycle_target_driver.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/ports/icclc_current_component_snapshot_source.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kCCLCOpenRequestApiVersion = "manyforge.cclc_open_request.v1";
inline constexpr std::string_view kCCLCOpenResponseApiVersion = "manyforge.cclc_open_response.v1";
inline constexpr std::size_t kMaximumCCLCOpenRequestBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumCCLCOpenResponseBytes = 8U * 1024U * 1024U;

/// One fresh, complete-domain route request template reconstructed only from
/// the selected retained bundle and current CCLC facts. A caller adds its own
/// request context; it must not edit or merge any field in this value.
struct CCLCRouteSelectionTemplateDTO final {
  DesiredControlConfigurationDTO desired_configuration;
  ExpectedActiveControlConfigurationDTO expected_active_control_configuration;
  std::vector<RouteSelectionComponentExpectationDTO> component_expectations;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCRouteSelectionTemplateDTO&,
                         const CCLCRouteSelectionTemplateDTO&) = default;
};

/// Retained/current route templates relevant to one exact component. Empty is
/// a truthful statement that the component has no retained selectable route.
struct CCLCRouteSelectionTemplateSetDTO final {
  ControllableComponentId controllable_component_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::vector<CCLCRouteSelectionTemplateDTO> templates;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const CCLCRouteSelectionTemplateSetDTO&,
                         const CCLCRouteSelectionTemplateSetDTO&) = default;
};

class ICCLCRouteSelectionTemplateSource {
 public:
  virtual ~ICCLCRouteSelectionTemplateSource() = default;

  [[nodiscard]] virtual Result<CCLCRouteSelectionTemplateSetDTO> ReadCurrentRouteSelectionTemplates(
      const ControllableComponentId& component_id) const = 0;
};

/// Narrow caller-neutral semantic surface shared by HTTP, ROS, CLI, Behavior,
/// and other open clients. Implementations own no policy or runtime truth;
/// every method delegates to the sole CCLC owner or its verified read source.
class ICCLCOpenRequestPort {
 public:
  virtual ~ICCLCOpenRequestPort() = default;

  [[nodiscard]] virtual RequestOwnerEpoch ReadRequestOwnerEpoch() const = 0;
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

  [[nodiscard]] RequestOwnerEpoch ReadRequestOwnerEpoch() const override;
  [[nodiscard]] Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) override;
  [[nodiscard]] Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent) override;
  [[nodiscard]] Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& transaction_id) override;
  [[nodiscard]] Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
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
  const ICCLCCurrentComponentSnapshotSource& current_component_source_;
  const ICCLCRouteSelectionTemplateSource& route_template_source_;
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
