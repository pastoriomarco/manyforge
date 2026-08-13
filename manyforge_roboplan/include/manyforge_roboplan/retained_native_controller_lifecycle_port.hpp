#pragma once

#include <atomic>
#include <manyforge_core/ports/icontroller_lifecycle_port.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <vector>

namespace manyforge::roboplan_backend {

/// Controller-lifecycle adapter for one immutable, direct, in-process native
/// control domain.
///
/// The native execution runtime has no separately mutable controller manager:
/// its sole controller and selected route exist for exactly the lifetime of
/// the bound backend.  Construction therefore seals the retained expected
/// domain against the complete physical RuntimeParticipantBinding set.  A
/// successful observation reports that construction-owned state with a fresh
/// source sequence; it never copies identity from the query or discovers a
/// replacement route.  Transition requests fail known-no-effect because this
/// adapter cannot reconfigure its own in-process topology.
class RetainedNativeControllerLifecyclePort final
    : public core::IControllerLifecyclePort {
public:
  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedNativeControllerLifecyclePort>>
  Create(core::ControllerLifecycleExpectedDomain expected_domain,
         std::vector<core::RuntimeParticipantBinding> physical_bindings,
         core::IClock &clock, core::Duration freshness_duration_ns);

  RetainedNativeControllerLifecyclePort(
      const RetainedNativeControllerLifecyclePort &) = delete;
  RetainedNativeControllerLifecyclePort &
  operator=(const RetainedNativeControllerLifecyclePort &) = delete;

  [[nodiscard]] core::Result<core::ControllerLifecycleObservationDTO>
  Observe(const core::ControllerLifecycleObservationQueryDTO &query) override;

  [[nodiscard]] core::ControllerLifecycleTransitionResultDTO
  ApplyTransitionStep(
      const core::ControllerLifecycleTransitionIntentDTO &intent,
      std::stop_token cancellation) override;

private:
  RetainedNativeControllerLifecyclePort(
      core::ControllerLifecycleExpectedDomain expected_domain,
      core::ControllerLifecycleObservedConfigurationDTO active_configuration,
      std::vector<core::ControllerLifecycleObservedControllerDTO> controllers,
      std::vector<core::ControllerChainEdgeRefDTO> active_chain_edges,
      std::vector<core::ControllerLifecycleObservedEndpointDTO> endpoints,
      core::IClock &clock, core::Duration freshness_duration_ns);

  core::ControllerLifecycleExpectedDomain expected_domain_;
  core::ControllerLifecycleObservedConfigurationDTO active_configuration_;
  std::vector<core::ControllerLifecycleObservedControllerDTO> controllers_;
  std::vector<core::ControllerChainEdgeRefDTO> active_chain_edges_;
  std::vector<core::ControllerLifecycleObservedEndpointDTO> endpoints_;
  core::IClock &clock_;
  core::Duration freshness_duration_ns_;
  std::atomic<std::uint64_t> next_source_sequence_{1U};
};

} // namespace manyforge::roboplan_backend
