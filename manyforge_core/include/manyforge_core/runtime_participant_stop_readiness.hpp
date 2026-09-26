#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/resolved_stop_base_plan.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <utility>

namespace manyforge::core {

/// Complete readiness input for preparing Stop products for one exact runtime
/// participant. CCLC supplies the repository-verified base plan and the
/// lifecycle generation from the same reconciled component/configuration
/// closure; the Kernel never discovers either value on the Stop path.
struct RuntimeParticipantStopReadinessDTO final {
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ControllerLifecycleExpectedDomainDTO expected_controller_domain;
  ResolvedStopBasePlanDTO resolved_stop_base_plan;
  /// Exact saved timeout minus latency and settling. Absent retains the
  /// qualified policy defaults. Cold authority only, not another deadline.
  std::optional<std::int64_t> maximum_controlled_stop_duration_ns{std::nullopt};

  friend bool operator==(const RuntimeParticipantStopReadinessDTO&,
                         const RuntimeParticipantStopReadinessDTO&) = default;
};

/// Sealed readiness value accepted by Kernel composition and Prepare.
///
/// Construction verifies the content-addressed base plan and joins every
/// participant-owned current configuration fact. A public operation or Stop
/// caller cannot construct or substitute this value.
class RuntimeParticipantStopReadiness final {
 public:
  RuntimeParticipantStopReadiness(const RuntimeParticipantStopReadiness&) = default;
  RuntimeParticipantStopReadiness& operator=(const RuntimeParticipantStopReadiness&) = default;
  RuntimeParticipantStopReadiness(RuntimeParticipantStopReadiness&&) noexcept = default;
  RuntimeParticipantStopReadiness& operator=(RuntimeParticipantStopReadiness&&) noexcept = default;

  [[nodiscard]] const RuntimeParticipantStopReadinessDTO& value() const noexcept { return value_; }

 private:
  explicit RuntimeParticipantStopReadiness(RuntimeParticipantStopReadinessDTO value)
      : value_(std::move(value)) {}

  RuntimeParticipantStopReadinessDTO value_;

  friend Result<RuntimeParticipantStopReadiness> verify_runtime_participant_stop_readiness(
      RuntimeParticipantStopReadinessDTO, const RuntimeParticipantBinding&,
      const ControllerLifecycleExpectedDomain&);
  friend Result<RuntimeParticipantStopReadiness>
  rebind_runtime_participant_stop_readiness_generation(const RuntimeParticipantStopReadiness&,
                                                       ComponentLifecycleGeneration);
};

/// Re-issue already verified readiness for a CCLC lifecycle transition. The
/// verified base plan, participant and controller-domain joins are unchanged:
/// a Prepare/Enable/Disable advances only the component lifecycle generation,
/// so engines, route configurations and Stop-plan bytes are reused as-is.
[[nodiscard]] Result<RuntimeParticipantStopReadiness>
rebind_runtime_participant_stop_readiness_generation(const RuntimeParticipantStopReadiness& verified,
                                                     ComponentLifecycleGeneration generation);

/// Re-seal the same verified Stop mechanism to a successor Program/Scene
/// product while retaining its controller, route and lifecycle generation.
/// The independently verified controller domain must still be identical;
/// changing physical Stop facts requires a component replacement instead.
[[nodiscard]] Result<RuntimeParticipantStopReadiness>
rebind_runtime_participant_stop_readiness_product(
    const RuntimeParticipantStopReadiness& verified,
    const RuntimeParticipantBinding& predecessor,
    const RuntimeParticipantBinding& candidate,
    const ControllerLifecycleExpectedDomain& expected_domain);

/// Verify one CCLC-produced readiness value against a sealed runtime
/// participant. The verified base-plan bytes remain authoritative; this
/// function performs no repository read, endpoint lookup, or live mutation.
[[nodiscard]] Result<RuntimeParticipantStopReadiness> verify_runtime_participant_stop_readiness(
    RuntimeParticipantStopReadinessDTO value, const RuntimeParticipantBinding& participant,
    const ControllerLifecycleExpectedDomain& expected_controller_domain);

}  // namespace manyforge::core
