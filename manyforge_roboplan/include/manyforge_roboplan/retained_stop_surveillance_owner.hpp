#pragma once

#include <cstdint>
#include <manyforge_core/ports/istop_surveillance_observation_port.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/stop_fault_recovery.hpp>
#include <manyforge_core/types/resolved_stop_base_plan.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <memory>
#include <mutex>
#include <vector>

namespace manyforge::roboplan_backend {

/// One exact requirement/source/scope row copied from a repository-verified
/// participant Stop plan. The source owner never accepts a public selector.
struct RetainedStopSurveillanceObservationSource final {
  core::StopEvidenceRequirementDTO evidence_requirement;
  core::ResolvedStopEvidenceSourceDTO resolved_source;
  std::vector<core::ResourceId> observed_resources;

  friend bool
  operator==(const RetainedStopSurveillanceObservationSource &,
             const RetainedStopSurveillanceObservationSource &) = default;
};

/// Construction input joining one sealed physical binding, its retained state
/// channel, and only source rows copied from the same verified Stop plan.
struct RetainedStopSurveillanceSourceInput final {
  core::RuntimeParticipantBinding binding;
  std::shared_ptr<IRetainedObservedStateChannel> state_channel;
  std::vector<RetainedStopSurveillanceObservationSource> observation_sources;
};

/// Command-free native surveillance and recovery-facts owner. It reads only
/// retained addressed-state publications and never invokes a controller,
/// backend Stop, planner, discovery surface, or journal.
class RetainedStopSurveillanceOwner final
    : public core::IStopSurveillanceObservationPort {
public:
  explicit RetainedStopSurveillanceOwner(
      std::vector<RetainedStopSurveillanceSourceInput> sources);

  RetainedStopSurveillanceOwner(const RetainedStopSurveillanceOwner &) = delete;
  RetainedStopSurveillanceOwner &
  operator=(const RetainedStopSurveillanceOwner &) = delete;

  /// Validate a prospective route source table without publishing it.
  [[nodiscard]] static core::Result<void> ValidateSources(
      const std::vector<RetainedStopSurveillanceSourceInput> &sources);

  /// Atomically replace the complete physical source table after exact route
  /// publication/readback. Previously copied readers remain lifetime-safe.
  [[nodiscard]] core::Result<void>
  ReplaceSources(std::vector<RetainedStopSurveillanceSourceInput> sources);

  [[nodiscard]] core::StopSurveillanceObservationSampleDTO Observe(
      const core::StopSurveillanceObservationQueryDTO &query) noexcept override;

  /// Build only owner-issued observation/configuration facts. The Kernel adds
  /// its live Stop/slot/fence blockers before evaluating a clear.
  [[nodiscard]] core::Result<core::StopFaultRecoveryFactsDTO>
  BuildRecoveryFacts(const core::StopStatusDTO &status,
                     std::int64_t assessed_at_ns) noexcept;

private:
  mutable std::mutex mutex_;
  std::vector<RetainedStopSurveillanceSourceInput> sources_;
};

} // namespace manyforge::roboplan_backend
