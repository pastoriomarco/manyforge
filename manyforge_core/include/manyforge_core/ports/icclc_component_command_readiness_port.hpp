#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <cstdint>
#include <optional>

namespace manyforge::core {

/// Kernel projection of one component's command eligibility.
///
/// This is separate from domain route/Stop readiness and from cell admission:
/// ordinary dispatch requires all three. It is bounded runtime state, never a
/// second durable authority; CCLC's journal remains the lifecycle record.
struct ComponentCommandReadinessDTO final {
  ControllableComponentId controllable_component_id;
  /// Absent until CCLC or verified composition publishes a generation.
  std::optional<ComponentLifecycleGeneration> component_lifecycle_generation;
  bool command_eligible{false};

  friend bool operator==(const ComponentCommandReadinessDTO&,
                         const ComponentCommandReadinessDTO&) = default;
};

/// The positive authority under which a component result is published.
/// Absence of another claim is never permission to enable commands.
enum class ComponentReadinessAuthorityDTO : std::uint8_t {
  /// The exact held CCLC lifecycle claim named by the transaction id.
  kLifecycleClaim = 0,
  /// The Manager's held Apply fence covering every resource of the component
  /// (candidate initialization under the parent Apply).
  kParentApplyFence,
  /// Kernel route-recovery exclusion held during owner attachment/restart,
  /// which refuses every ordinary claim while facts are reconstructed.
  kRestartExclusion,
};

/// One complete component result. Publication re-seals every runtime
/// participant of the component at `component_lifecycle_generation`, reusing
/// its engines, route configurations and Stop-plan bytes, then sets
/// eligibility for all of its domains together.
struct ComponentCommandReadinessPublicationDTO final {
  ComponentReadinessAuthorityDTO authority{ComponentReadinessAuthorityDTO::kLifecycleClaim};
  /// Required exactly for `kLifecycleClaim`.
  std::optional<LifecycleTransactionId> lifecycle_transaction_id;
  /// Required exactly for `kParentApplyFence`: the parent Apply's own held
  /// fence. Coverage by any other fence is not this Apply's authority.
  std::optional<ConfigurationFenceId> parent_apply_fence_id;
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  bool command_eligible{false};

  [[nodiscard]] bool locally_valid() const noexcept {
    const bool claim = authority == ComponentReadinessAuthorityDTO::kLifecycleClaim;
    const bool fence = authority == ComponentReadinessAuthorityDTO::kParentApplyFence;
    return claim == lifecycle_transaction_id.has_value() &&
           (!lifecycle_transaction_id.has_value() || !lifecycle_transaction_id->empty()) &&
           fence == parent_apply_fence_id.has_value() &&
           (!parent_apply_fence_id.has_value() || !parent_apply_fence_id->empty()) &&
           (claim || authority == ComponentReadinessAuthorityDTO::kParentApplyFence ||
            authority == ComponentReadinessAuthorityDTO::kRestartExclusion) &&
           !controllable_component_id.empty() && component_lifecycle_generation.nonzero();
  }

  friend bool operator==(const ComponentCommandReadinessPublicationDTO&,
                         const ComponentCommandReadinessPublicationDTO&) = default;
};

/// CCLC's seam to the Kernel's component command eligibility. Adapters own
/// provider work and observations; CCLC owns aggregate completion; the
/// Kernel owns command permission.
class ICCLCComponentCommandReadinessPort {
 public:
  virtual ~ICCLCComponentCommandReadinessPort() = default;

  /// Close eligibility under the exact held lifecycle claim before any
  /// transition effect. Returns the Kernel readback.
  [[nodiscard]] virtual Result<ComponentCommandReadinessDTO> CloseComponentCommandEligibility(
      const LifecycleTransactionId& lifecycle_transaction_id,
      const ControllableComponentId& controllable_component_id) = 0;

  /// Publish one complete component result and return the Kernel readback.
  [[nodiscard]] virtual Result<ComponentCommandReadinessDTO> PublishComponentCommandReadiness(
      const ComponentCommandReadinessPublicationDTO& publication) = 0;

  /// Bounded Kernel readback, used to verify containment without authority.
  [[nodiscard]] virtual Result<ComponentCommandReadinessDTO> ReadComponentCommandReadiness(
      const ControllableComponentId& controllable_component_id) const = 0;
};

}  // namespace manyforge::core
